# immich — photo slideshow from an Immich album

Show photos from an [Immich](https://immich.app) album on an ESPHome display. Every update interval
the component asks the Immich server for one random image from the album, points an
[`online_image`](https://esphome.io/components/image/online_image/) at that photo's preview and
downloads it. The slideshow is idle until it is started with `immich.start`, so it works well as a
screensaver.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/remcom/esphome-component
      ref: main
    components:
      - immich
```

Requires ESPHome 2026.10.0 or newer.

## Configuration

The component needs an `http_request` component and an `online_image` to load the photos into. The
`url` of the `online_image` is a placeholder: the component replaces it with the address of each
photo it shows.

```yaml
http_request:
  timeout: 10s

image:
  - platform: online_image
    id: slideshow_image
    url: http://immich.local:2283/  # replaced by the component
    format: JPEG
    type: RGB565
    resize: 320x240

immich:
  id: my_immich
  url: http://immich.local:2283
  api_key: !secret immich_api_key
  album_id: 9c5e2bd7-3b0f-4bb1-90b4-4a2e0e6a7b3d
  image_id: slideshow_image
  update_interval: 30s

display:
  - platform: ili9xxx
    # ...
    lambda: |-
      it.image(0, 0, id(slideshow_image));
```

The album id is the UUID at the end of the album's web address
(`https://immich.example.com/albums/<album_id>`). Create the API key under *Account Settings →
API Keys* as a user who can see the album; a key with restricted permissions must be allowed to
search assets and to view them.

### Options

| Key | Default | Description |
|-----|---------|-------------|
| `url` | — | Base address of the Immich server, e.g. `http://immich.local:2283` |
| `api_key` | — | Immich API key, sent as `x-api-key` with every request (search and photo download) |
| `album_id` | — | UUID of the album to show |
| `image_id` | — | The `online_image` the photos are loaded into |
| `http_request_id` | — | The `http_request` component to use, when there is more than one |
| `update_interval` | `30s` | Time each photo is shown while the slideshow runs |

Multiple `immich` instances can be configured, for example one per album or per display.

### Actions and conditions

```yaml
on_...:
  - immich.start: my_immich       # show a photo now and rotate every update_interval
  - immich.stop: my_immich        # stop rotating; the last photo stays on the image
  - immich.next_image: my_immich  # show the next photo once, without starting or stopping
  - if:
      condition:
        immich.is_running: my_immich
      then:
        # ...
```

Example: start the slideshow after a minute without touches, stop it on the next touch.

```yaml
touchscreen:
  - platform: ...
    on_release:
      - immich.stop: my_immich
      - script.execute: screensaver_timer

script:
  - id: screensaver_timer
    mode: restart
    then:
      - delay: 60s
      - immich.start: my_immich
```

## How it works

1. The component sends `POST /api/search/random` with the album id, `type: IMAGE` (videos are
   skipped) and `size: 1`, leaving out EXIF and people data so the response stays small. Only the
   asset id is parsed from the response.
2. It sets the `online_image` URL to `/api/assets/<id>/thumbnail?size=preview` and starts the
   download. The `online_image` decodes and resizes the photo in its own loop and triggers its
   `on_download_finished` / `on_error` automations as usual.
3. If the previous photo is still loading when the next one is due, that turn is skipped.

A failed request, an HTTP error, an unexpected response or an empty album sets a warning status on
the component (with a log message); the next successful photo clears it.

## Scope / Limitations

- The search request is synchronous: it blocks the main loop until the Immich server answers, at
  most for the `http_request` timeout. The photo download itself does not block.
- The search response may be at most 4 KiB; that is several times what one asset without EXIF and
  people data takes.
- Immich preview images are large (1440 px on the long side by default). Decoding them needs a lot
  of RAM; use an ESP32 with PSRAM and set `resize` on the `online_image` to the display size.
- Only images are shown; videos in the album are skipped. Photos are picked at random, so one can
  come up again before all photos of the album have been shown.
- The component is compile-tested on ESP32 (ESP-IDF and Arduino). `image:` requires a display,
  so there is no host compile test.

## Running tests

```bash
pytest tests/immich/test_schema.py -v                       # schema tests, no hardware
esphome compile tests/components/immich/test.esp32-idf.yaml
```
