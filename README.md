# vk_video

A sample demo using FFmpeg and Vulkan video coding APIs to encode and decode videos.

Current status:
- [x] Decoding
- [ ] Encoding
- [ ] Presenting

## Build

First, clone the repo with submodules:
```sh
git clone https://github.com/btmxh/vk_video.git --recurse-submodules
cd vk_video
```

Then, install Vulkan and FFmpeg (libraries). After that configure and build:
```sh
cmake -S. -Bbuild

# or if you built ffmpeg from scratch for a better debugging experience,
# override the pkg-config paths
cmake -S. -Bbuild -DPC_FFMPEG_LIBRARY_DIRS=/usr/local/lib -DPC_FFMPEG_INCLUDE_DIRS=/usr/local/include

# build
cmake --build build -j
```

Finally run the application against a video file:
```
build/app/vkvideo_app ~/Videos/untitled.mp4
```

Currently, the program will just dump the first 10 frames (in grayscale) from the video and exit.


