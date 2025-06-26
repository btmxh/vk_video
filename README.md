# vk_video

A sample demo using FFmpeg and Vulkan video coding APIs to encode and decode videos.

Current status:
- [x] Decoding
- [x] Presenting
- [x] Audio
- [x] Encoding

## Build

First, clone the repo with submodules:
```sh
git clone https://github.com/btmxh/vk_video.git --recurse-submodules
cd vk_video
# sync shaderc dependencies
./extern/shaderc/utils/git-sync-deps
```

Since `shaderc` requires other dependencies, 

Then, install Vulkan and FFmpeg (libraries). After that configure and build:
```sh
# if you built FFmpeg from scratch for a better debugging experience,
# override the pkg-config paths for FFmpeg
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig/

# configure
cmake -S. -Bbuild

# build
cmake --build build -j
```

Run the video player for an video file:
```
build/app/vkvideo_player ~/Videos/untitled.mp4
```

Run the transcoding example (re-encode a given video using GPU):
```
build/app/vkvideo_transcode ~/Videos/untitled.mp4 output.mp4
```

## Pending issues

### Video player

- [ ] Running with GPU-assisted validation gives validation errors.
      [Maybe related](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/10185).

### Transcoding example

- [ ] `hw_frames_ctx` is not cleaned up properly.
- [ ] Race conditions on the output render target frame.
