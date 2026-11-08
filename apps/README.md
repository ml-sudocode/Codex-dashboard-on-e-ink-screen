# Writing an app

An app is a folder here, `apps/<name>/`, and an ESP-IDF component of its own: its sources, its assets, the tools
that make them (`tools/`), and its home screen icon. It talks to the badge only through the OS headers,
`#include "badge/badge.h"` (`components/os/include/badge/`, one header per service, each documented at the top).
How the OS works underneath: [docs/os.md](../docs/os.md).

## The folder
```
apps/hello/
  CMakeLists.txt
  hello.c
```
`CMakeLists.txt` lists the sources and everything the app includes beyond the OS:
```cmake
idf_component_register(SRCS "hello.c" PRIV_REQUIRES os)
```
Common extras: `stb` (stb_image: JPEG, PNG, zlib), `esp_http_client` + `mbedtls` (HTTPS with
`esp_crt_bundle_attach`), `esp_timer`, `esp_wifi` (ESP-NOW), `esp_netif`, `cricket`. `EMBED_FILES` puts an asset in
flash (see `video/`). A managed dependency goes in the app's own `idf_component.yml` (see `settings/`, `update/`).

`hello.c` ends with the app's descriptor, named `app_<name>`:
```c
#include <string.h>
#include "badge/badge.h"

static void run(void)
{
    ui_keys(NULL, NULL, NULL, "Home"); // what each button does, shown beside it in the top bar
    memset(fb, 0, sizeof fb);
    text(48, TOP + 48, 4, "Hello", true);
    ui_frame(1);
    ui_close(); // the screen sits still
    while (btn_get(-1) != BTN_B) {}
}

static void icon(int x, int y) // ICON x ICON px at (x, y): white on the black base
{
    icon_base(x, y);
    box(x + 48, y + 48, 80, 80, false);
}

const app_t app_hello = {"Hello", icon, run};
```
Then add it to `main/apps.c`: one `extern` and one line in `apps[]`, where it should sit on the home screen.

## The rules
- **`run()` takes over the badge** on the home screen's task (16 KB stack). Returning goes back home; most apps
  never return. Holding MENU + and MENU - together for 0.4 s restarts the badge into the home screen from any app,
  so a game must not need that chord. The press that opened the app has been flushed, and the panel starts all white
  with no session open.
- **Draw below `TOP`.** Rows 0..`TOP`-1 (48 px) are the OS top bar: button labels, the app's name, username, OS
  version, battery. `ui_frame` draws it over `fb`.
- **Label the buttons** with `ui_keys(plus, minus, a, b)`: each label shows beside its button (MENU + above A on the
  left, MENU - above B on the right). Every app starts with none.
- **Frames.** Draw into `fb` (960x672, 1 bit, 1 = black) with `box`, `outline`, `text`, then `ui_frame(1)`. A frame
  is a calm whole-screen update, ~0.27 s, ~3.5-4 frames/s at best: only the pixels that changed move, nothing
  flashes. `ui_frame` returns while the panel pulls, so draw the next frame meanwhile; run game logic on its own
  clock (a second task that calls `ui_frame` with the latest state, as `snake/` does), not one step per frame. An
  unchanged frame costs nothing. Ink is dark red, erased ink leaves a light ghost: `ui_wipe()` (~2.8 s) at scene
  changes. `ui_close()` when the screen will sit still.
- **Tasks** that call `ui_frame`, `ui_close` or `ui_wipe` need a stack of 6 KB or more: the frame is sent from the
  caller's task.
- **Never run the panel's slow refresh** (~35 s, true black and red): the owner does not want it on the badge.
- **Wi-Fi and ranging.** The OS keeps ranging on every screen, so other badges' Radar sees this one. `wifi_connect()`
  joins the saved network (Settings, the event Wi-Fi by default) and `wifi_on()` gives a bare radio for ESP-NOW: both
  stop ranging. Done with the network but staying in the app: `wifi_off()`, then `ranging_start()`. Back home the OS
  gives the radio back by itself. Never use ESP-NOW while ranging runs.
- **USB.** On the home screen the badge must always be flashable: an app that makes USB a host and returns gets the
  badge restarted; better, restart it yourself when done.
- **Settings** are `cfg_get` / `cfg_set` strings; the keys in use are listed in `badge/settings.h`. A new key needs a
  field in the Settings app to be edited.
- **Sound** (`snd_*`) and the **LEDs** are stopped and turned off when the app returns home; the LEDs also at boot.
- Keep it small and in the style of the code around it. Never commit `.bin` files or personal data.
