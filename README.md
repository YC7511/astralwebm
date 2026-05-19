# astralwebm

Axis ACAP FastCGI app for on-demand, token-gated WebM streaming.

RTSP source pattern used by the app:

`rtsp://127.0.0.1/axis-media/media.amp?camera=<N>&videocodec=h264&audio=0` (`N` is 1-99)

## Build

```bash
make
```
