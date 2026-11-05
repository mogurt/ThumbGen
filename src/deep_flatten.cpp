#include "deep_flatten.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
namespace thumbgen::detail {
namespace {
std::size_t checked_add(std::size_t a,std::size_t b) {
    if(b>std::numeric_limits<std::size_t>::max()-a)throw DeepFailure("deep_workspace_limit_exceeded","Deep workspace size overflows");
    return a+b;
}
std::size_t checked_mul(std::size_t a,std::size_t b) {
    if(a && b>std::numeric_limits<std::size_t>::max()/a)throw DeepFailure("deep_workspace_limit_exceeded","Deep workspace size overflows");
    return a*b;
}
std::size_t leaves(std::size_t n) {std::size_t result=1;while(result<n)result=checked_mul(result,2);return result;}
void validate(const DeepSample& s) {
    if(!std::isfinite(s.z) || !std::isfinite(s.zback) || s.z<0 || s.zback<0)
        throw DeepFailure("deep_value_invalid","Deep depths must be finite and nonnegative");
    for(int c=0;c<3;++c)if(!std::isfinite(s.colour[c]) || !std::isfinite(s.alpha[c]) || s.alpha[c]<0 || s.alpha[c]>1)
        throw DeepFailure("deep_value_invalid","Deep colour must be finite and alpha must be in [0,1]");
}
}
DeepFlattener::DeepFlattener(DeepLimits limits):limits_(limits) {
    if(!limits.workspace_bytes || limits.samples_per_pixel>std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("invalid Deep workspace/sample limits");
}
std::size_t DeepFlattener::workspace_bound(std::size_t n) {
    if(n<=1)return 0;
    auto samples=checked_mul(n,sizeof(DeepSample));
    auto events=checked_mul(checked_mul(n,2),sizeof(Event));
    auto nodes=checked_mul(checked_mul(leaves(n),2),sizeof(Node));
    // Include old storage while vector reserve grows. std::sort is in-place.
    return checked_mul(checked_add(checked_add(samples,events),nodes),2);
}
std::size_t DeepFlattener::retained_workspace_bytes() const noexcept {
    return samples_.capacity()*sizeof(DeepSample)+events_.capacity()*sizeof(Event)+tree_.capacity()*sizeof(Node);
}
DeepFlattener::Node DeepFlattener::join(const Node& a,const Node& b) {
    Node result;
    for(int c=0;c<3;++c)result.channel[c]={a.channel[c].density+b.channel[c].density,a.channel[c].emission+b.channel[c].emission,
        a.channel[c].opaque_colour+b.channel[c].opaque_colour,a.channel[c].opaque_count+b.channel[c].opaque_count};
    return result;
}
DeepFlattener::Node DeepFlattener::contribution(const DeepSample& s,bool volume) {
    Node result;double length=volume?double(s.zback)-s.z:1;
    for(int c=0;c<3;++c) {
        auto& value=result.channel[c];double alpha=s.alpha[c],colour=s.colour[c];
        if(alpha==1){value.opaque_count=1;value.opaque_colour=colour;continue;}
        double tau=-std::log1p(-alpha);
        value.density=tau/length;value.emission=(colour*(alpha>0?tau/alpha:1))/length;
    }
    return result;
}
void DeepFlattener::composite(FlatDeepPixel& result,std::array<double,3>& transmission,const Node& node,double extent) {
    for(int c=0;c<3;++c) {
        const auto& value=node.channel[c];double alpha,colour,through;
        if(value.opaque_count){alpha=1;through=0;colour=value.opaque_colour/value.opaque_count;}
        else {
            double tau=value.density*extent;alpha=-std::expm1(-tau);through=std::exp(-tau);
            colour=value.emission*(value.density>0?alpha/value.density:extent);
        }
        double transmittance=transmission[c];
        result.colour[c]+=transmittance*colour;result.alpha[c]+=transmittance*alpha;
        transmission[c]*=through;
    }
}
FlatDeepPixel DeepFlattener::flatten(const DeepSample* source,std::size_t count) {
    if(count>limits_.samples_per_pixel)throw DeepFailure("deep_sample_limit_exceeded","Deep pixel sample limit exceeded");
    if(count && !source)throw std::invalid_argument("null Deep samples");
    if(workspace_bound(count)>limits_.workspace_bytes)throw DeepFailure("deep_workspace_limit_exceeded","Deep workspace limit exceeded");
    for(std::size_t i=0;i<count;++i)validate(source[i]); // Validate even samples hidden by an opaque foreground.
    FlatDeepPixel result;
    if(count<=1){if(count)for(int c=0;c<3;++c){result.colour[c]=source[0].colour[c];result.alpha[c]=source[0].alpha[c];result.transmission[c]=1.0-source[0].alpha[c];}return result;}
    auto leaf_count=leaves(count);
    samples_.reserve(count);events_.reserve(count*2);tree_.reserve(leaf_count*2);
    if(retained_workspace_bytes()>limits_.workspace_bytes)throw DeepFailure("deep_workspace_limit_exceeded","Deep allocator capacity exceeds workspace limit");
    samples_.assign(source,source+count);events_.clear();tree_.assign(leaf_count*2,Node{});
    for(auto& sample:samples_) {
        sample.zback=std::max(sample.z,sample.zback); // A reversed interval is a point under the OpenEXR convention.
        if(sample.z==0)sample.z=0;
        if(sample.zback==0)sample.zback=0;
        for(int c=0;c<3;++c){if(sample.colour[c]==0)sample.colour[c]=0;if(sample.alpha[c]==0)sample.alpha[c]=0;}
    }
    // A canonical order makes interval sums and opaque ties independent of file sample order.
    std::sort(samples_.begin(),samples_.end(),[](const auto& a,const auto& b){return std::tie(a.z,a.zback,a.alpha,a.colour)<std::tie(b.z,b.zback,b.alpha,b.colour);});
    for(std::size_t i=0;i<count;++i) {
        auto& sample=samples_[i];
        if(sample.zback==sample.z)events_.push_back({sample.z,Kind::point,i});
        else {events_.push_back({sample.z,Kind::begin,i});events_.push_back({sample.zback,Kind::end,i});}
    }
    std::sort(events_.begin(),events_.end(),[](const auto& a,const auto& b){return std::tie(a.depth,a.kind,a.sample)<std::tie(b.depth,b.kind,b.sample);});
    float previous=events_.front().depth;
    auto& transmission=result.transmission;
    for(std::size_t i=0;i<events_.size();) {
        float depth=events_[i].depth;
        if(depth>previous)composite(result,transmission,tree_[1],double(depth)-previous);
        Node points;bool has_points=false;
        do {
            const auto& event=events_[i];
            if(event.kind==Kind::point){points=join(points,contribution(samples_[event.sample],false));has_points=true;}
            else {
                auto position=leaf_count+event.sample;tree_[position]=event.kind==Kind::begin?contribution(samples_[event.sample],true):Node{};
                while(position>1){position/=2;tree_[position]=join(tree_[position*2],tree_[position*2+1]);}
            }
            ++i;
        }while(i<events_.size() && events_[i].depth==depth);
        if(has_points)composite(result,transmission,points,1);
        previous=depth;
    }
    for(int c=0;c<3;++c)if(!std::isfinite(result.colour[c]) || !std::isfinite(result.alpha[c]))
        throw DeepFailure("deep_value_invalid","Deep flattened result is not finite");
    for(auto& alpha:result.alpha)alpha=std::clamp(alpha,0.0,1.0);
    return result;
}
}
