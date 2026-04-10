# vcpkg WebRTC Sample

To demonstrate how to use WebRTC with CMake and vcpkg, this sample implements a simple command-line application that connects to an [AppRTC](https://github.com/webrtc/apprtc)-compatible signaling server.

The original public AppRTC demo service at `https://appr.tc` is no longer usable. In practice, you should provide your own AppRTC-compatible room/signaling server and pass it via `--room_server_url`. The E2E test also requires `WEBRTC_APPRTC_TEST_SERVER_URL` to point to that server.

## Configure

```bash
cmake -S . -B cmake-build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
cmake --build cmake-build
ctest --test-dir cmake-build --output-on-failure
```

If you want the input-validation and E2E tests to run, provide an MP4 path during
configure:

```bash
cmake -S . -B cmake-build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DWEBRTC_APPRTC_SAMPLE_VIDEO=/absolute/path/to/sample.mp4
```

## Run

```bash
./cmake-build/webrtc_apprtc_cli \
  --room_id=my-room \
  --video_path=/absolute/path/to/sample.mp4
```

Useful flags:

- `--room_server_url`
- `--room_id`
- `--timeout_seconds`
- `--call_duration_seconds`
- `--video_path`
- `--audio_frequency_hz`
- `--stats_interval_seconds`

## E2E

To run the two-process E2E test:
- pass `-DWEBRTC_APPRTC_SAMPLE_VIDEO=/absolute/path/to/sample.mp4` at configure time
- set `WEBRTC_APPRTC_TEST_SERVER_URL` in the environment when running `ctest`

For example:

```bash
WEBRTC_APPRTC_TEST_SERVER_URL=https://your-apprtc-server.example.com \
ctest --test-dir cmake-build --output-on-failure
```
