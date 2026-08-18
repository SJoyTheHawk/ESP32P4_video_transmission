# Phase 6 Arduino Test

The `v3.0-phase6-arduino` sketch starts Espressif's `esp_http_server` on TCP
port 80 after Wi-Fi and camera initialization. The routes are:

- `GET /api/photo/metadata`
- `GET /api/photo/latest.jpg`
- `POST /api/photo/capture`

The capture POST only enqueues work and returns `202 Accepted`. The worker runs
the 1080p controller transaction outside the HTTP handler, publishes the JPEG
after baseline restoration, and returns the API to `ready` or `error`.

Upload with the board's detected 16 MB flash profile:

- Flash Size: `16MB`
- Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)`

The serial monitor must show:

```text
implementation version=v3.0-phase6-arduino
HTTP photo API ready: port=80 routes=/api/photo/*
```

With the board IP substituted, exercise the empty and asynchronous paths:

```sh
curl -i http://192.168.1.91/api/photo/metadata
curl -i http://192.168.1.91/api/photo/latest.jpg
curl -i -X POST -H 'Content-Type: application/json' \
  -d '{}' http://192.168.1.91/api/photo/capture
curl -i -X POST -H 'Content-Type: application/json' \
  -d '{"unexpected":true}' http://192.168.1.91/api/photo/capture
```

Expected initial responses are `404` for metadata/latest (no retained photo),
`202` for the valid capture request, and `400` for the unsupported body. A
second POST while the first is queued or running returns `409`. Once a capture
has completed, use the returned ETag from `latest.jpg` with `If-None-Match` and
verify `304 Not Modified`. The JPEG body is sent in bounded chunks while the
PhotoStore reference remains held.

To exercise the unavailable case without exposing a network fault endpoint,
send `u` in the serial monitor. The API remains up and all three routes must
return `503 Service Unavailable`:

```sh
curl -i http://192.168.1.91/api/photo/metadata
curl -i http://192.168.1.91/api/photo/latest.jpg
curl -i -X POST -H 'Content-Type: application/json' \
  -d '{}' http://192.168.1.91/api/photo/capture
```

Send `r` in the serial monitor to restore the API to `ready`, then repeat the
metadata and RTSP checks.

Repeat the Phase 4 RTSP and RTP tests afterward; HTTP photo capture may create a
measured RTSP gap while the camera switches modes, but it must restore the
800x800 stream without reboot.
