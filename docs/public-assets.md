# 示例素材

[文档目录](README.md) · [English](en/public-assets.md)

想试试 EXR、多层或透明图片，可以从下面的公开来源选择素材。下载前请查看相应许可。

| 来源 | 内容 |
| --- | --- |
| [OpenEXR 示例图片](https://github.com/AcademySoftwareFoundation/openexr-images) | 普通 EXR、Deep、多 part 和序列 |
| [Psyop Cryptomatte](https://github.com/Psyop/Cryptomatte) | Cryptomatte 示例与格式说明 |
| [Poly Haven](https://polyhaven.com/) | CC0 HDRI 和纹理 |

图片下载链接与许可见[图片来源清单](performance/public-assets.json)。

## 首页预览配图

| 配图 | 公开素材 | 处理方式 |
| --- | --- | --- |
| Cryptomatte | [Psyop 的 Cornell Box 与 Bunny 示例](https://github.com/Psyop/Cryptomatte/tree/968d5e4b6171e29ba5f89d554117132a164e747e/sample_images) | 分别读取 `uCryptoWildcard` 与 `uCryptoAsset` 完整家族；两张图来自不同场景 |
| Depth | [OpenEXR Beachball 第一帧](https://github.com/AcademySoftwareFoundation/openexr-images/tree/e38ffb0790f62f05a6f083a6fa4cac150b3b7452/Beachball) | 同一文件的 RGB 与 Z 通道，深度范围为源单位 9–11，黑色近、白色远 |
| OCIO | [OpenEXR StillLife](https://github.com/AcademySoftwareFoundation/openexr-images/tree/e38ffb0790f62f05a6f083a6fa4cac150b3b7452/ScanLines) | 输入声明为 Linear Rec.709 (sRGB)，显示为 sRGB；对比 Un-tone-mapped 与 ACES 1.0 - SDR Video |

配图由 ThumbGen 0.1.1 生成，按比例排版，不裁切、不另行调色。棋盘格表示透明区域。OCIO 使用内置 `ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4` 配置；两种 View 都是可选的显示方式，不是“错误”与“正确”的对比。Depth 属于数据预览，不应用 OCIO。

[图片设置与来源校验值](assets/features/sources.json) · [Psyop 许可](assets/features/Cryptomatte-BSD-3-Clause.txt) · [OpenEXR 许可](assets/features/OpenEXR-BSD-3-Clause.txt)

不要上传保密项目、未获许可的素材或项目色彩配置。反馈问题时，一份可分享的小文件通常最有帮助。
