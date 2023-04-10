# UE5-ffmpeg
Record game screen and push RTMP in Unreal Engine 5.0.3

# 插件
将`Plugins`文件夹下的插件拷贝入工程下的`Plugins`文件夹（没有就自行创建同名文件夹）

# 注意事项
~~目前不支持DirectX 12，须在Project Setting下搜索`RHI`将`DX 12`改为`DX 11`~~

目前已支持DirectX 12 和 Vulkan，对ffmpeg研究较少所以勉强能用，但在测试中发现录制的视频有加速的问题，还在解决

增加了Linux支持，为了平衡大小（Github不准单文件超过50M）去除了调试符号，在交叉编译下打包通过，但还是有废弃函数没有修正，未经测试

# 引用
[原项目README](./README_old.md)
