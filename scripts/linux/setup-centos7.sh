#!/usr/bin/env bash
set -euo pipefail
test "$(id -u)" = 0
grep -qx 'CentOS Linux release 7.9.2009 (Core)' /etc/centos-release
mkdir -p /etc/yum.repos.d/original
for repo in /etc/yum.repos.d/CentOS-*.repo; do
    test ! -f "$repo" || mv "$repo" /etc/yum.repos.d/original/
done
cat > /etc/yum.repos.d/ThumbGen-Vault.repo <<'EOF'
[base]
name=CentOS 7.9.2009 Base Vault
baseurl=https://vault.centos.org/7.9.2009/os/$basearch/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
[updates]
name=CentOS 7.9.2009 Updates Vault
baseurl=https://vault.centos.org/7.9.2009/updates/$basearch/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
[extras]
name=CentOS 7.9.2009 Extras Vault
baseurl=https://vault.centos.org/7.9.2009/extras/$basearch/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
[sclo-rh]
name=CentOS 7.9.2009 SCL RH Vault
baseurl=https://vault.centos.org/7.9.2009/sclo/$basearch/rh/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-SIG-SCLo
EOF
id builder >/dev/null 2>&1 || useradd -m -s /bin/bash builder
# Keep password locked; administrative commands use wsl -u root.
printf '[user]\ndefault=builder\n' > /etc/wsl.conf
printf '\ntimeout=15\nretries=1\n' >> /etc/yum.conf
curl --fail --connect-timeout 8 --max-time 15 -I https://vault.centos.org/7.9.2009/os/x86_64/repodata/repomd.xml
timeout 240 yum -y --disablerepo=sclo-rh install centos-release-scl-rh
for repo in /etc/yum.repos.d/CentOS-*.repo; do
    test ! -f "$repo" || mv "$repo" /etc/yum.repos.d/original/
done
timeout 600 yum -y install devtoolset-11-gcc devtoolset-11-gcc-c++ devtoolset-11-binutils rh-git227-git make curl tar gzip bzip2 xz patch ca-certificates zlib-devel openssl-devel bzip2-devel libffi-devel xz-devel readline-devel sqlite-devel
getconf GNU_LIBC_VERSION
