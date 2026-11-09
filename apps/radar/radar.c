// Radar: badges running badge os nearby, by name (or the last 4 hex digits of the MAC) and distance (OS ranging,
// Wi-Fi FTM). With 2+ measured badges (3 with this one) it also draws a 2D map centred on this badge, fitted to
// every shared distance. No compass: the map's rotation and mirror are arbitrary (kept steady between frames).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "badge/badge.h"
#include "esp_attr.h"

#define NODES 32       // me + up to 31 badges
#define MAP_X 384      // map area: x MAP_X..W, y TOP..H
#define MAP_R 270      // px from the map centre to the outer ring
#define SNAP 6         // px grid for dots: small wobble in the fit does not redraw the panel
#define FRAME_MS 100   // poll for new distances every 0.1 s; while the screen keeps changing, ui_frame sets the pace

typedef struct { uint8_t mac[6]; float x, y; } node_t;

static node_t nd[NODES]; // [0] = me at (0, 0); positions in metres, kept between frames
static int nn;
// In PSRAM: internal RAM is kept for Wi-Fi and TLS (as the OS keeps fb).
EXT_RAM_BSS_ATTR static range_t rg[1024]; // 32 badges: 992 directed pairs
EXT_RAM_BSS_ATTR static float dist[NODES][NODES], sum[NODES][NODES]; // metres, dist < 0 unknown
EXT_RAM_BSS_ATTR static int cnt[NODES][NODES];

// What a badge is called on screen: its username if known (mine from Settings), else its 4-hex id. The MAC stays the key.
static void id_of(const uint8_t *mac, char *s)
{
    uint8_t me[6];
    ranging_me(me);
    if (memcmp(mac, me, 6) ? !ranging_name(mac, s) : cfg_get("name", s, 13) <= 0) sprintf(s, "%02X%02X", mac[4], mac[5]);
}

static int find(const node_t *v, int n, const uint8_t *mac)
{
    for (int i = 0; i < n; i++)
        if (!memcmp(v[i].mac, mac, 6)) return i;
    return -1;
}

// Fit positions to the known distances, me pinned at the origin. Each badge moves to the average of where each
// known distance alone would put it (a stress-majorization step); starting from last frame keeps the map steady.
static void fit(int iters)
{
    for (int it = 0; it < iters; it++)
        for (int i = 1; i < nn; i++) {
            float sx = 0, sy = 0;
            int k = 0;
            for (int j = 0; j < nn; j++) {
                if (j == i || dist[i][j] < 0) continue;
                float dx = nd[i].x - nd[j].x, dy = nd[i].y - nd[j].y, l = sqrtf(dx * dx + dy * dy);
                if (l < 1e-3f) dx = 0.01f * (i - j), dy = 0.01f, l = sqrtf(dx * dx + dy * dy); // coincident: nudge apart
                sx += nd[j].x + dist[i][j] * dx / l, sy += nd[j].y + dist[i][j] * dy / l, k++;
            }
            if (k) nd[i].x = sx / k, nd[i].y = sy / k;
        }
}

static float stress(void) // sum of squared distance errors, m^2
{
    float e = 0;
    for (int i = 0; i < nn; i++)
        for (int j = i + 1; j < nn; j++)
            if (dist[i][j] >= 0) e += powf(hypotf(nd[i].x - nd[j].x, nd[i].y - nd[j].y) - dist[i][j], 2);
    return e;
}

// One frame of layout: refine last frame's map, then try one fresh start (each badge on its ring around me, angles
// shuffled by try) in case the map folded into a wrong local minimum; the fresh one is kept only if clearly better.
// ponytail: ~130k inner steps per frame at 32 badges; fewer fit iterations if that ever slows the frame rate.
static void layout(const int *cm, int try)
{
    fit(30);
    node_t keep[NODES];
    memcpy(keep, nd, sizeof nd);
    float was = stress();
    for (int i = 1; i < nn; i++) {
        float a = (nd[i].mac[5] * 256 + nd[i].mac[4]) * 2 * (float)M_PI / 65536 + try * i * 2.4f;
        float d = cm[i] < 0 ? 1 : cm[i] / 100.0f;
        nd[i].x = d * cosf(a), nd[i].y = d * sinf(a);
    }
    fit(100);
    if (!(stress() < 0.8f * was - 0.01f)) memcpy(nd, keep, sizeof nd);
}

static void ring(int cx, int cy, int r) // dotted circle
{
    for (int a = 0; a < 360; a += 2) box(cx + r * cosf(a * (float)M_PI / 180), cy + r * sinf(a * (float)M_PI / 180), 2, 2, true);
}

static int snap(float v) { return (int)lroundf(v / SNAP) * SNAP; }

static void run(void)
{
    uint8_t me[6];
    ranging_me(me);
    ui_keys(NULL, NULL, "clear ghosts", "home");
    for (int frame = 0;; frame++) {
        int n = ranging_get(rg, 1024);

        // Nodes: me, then every badge I hear, measured ones first, nearest first; known positions carried over.
        node_t old[NODES];
        int on = nn;
        memcpy(old, nd, sizeof nd);
        nn = 1;
        memcpy(nd[0].mac, me, 6), nd[0].x = nd[0].y = 0;
        int cm[NODES] = {0}, measured = 0;
        for (int i = 0; i < n && nn < NODES; i++) {
            if (memcmp(rg[i].a, me, 6) || find(nd, nn, rg[i].b) >= 0) continue;
            int k = nn++;
            memcpy(nd[k].mac, rg[i].b, 6);
            cm[k] = rg[i].cm;
            measured += rg[i].cm >= 0;
            for (; k > 1 && (cm[k - 1] < 0 ? 1e9 : cm[k - 1]) > (cm[k] < 0 ? 1e9 : cm[k]); k--) { // insertion sort
                node_t t = nd[k]; nd[k] = nd[k - 1]; nd[k - 1] = t;
                int c = cm[k]; cm[k] = cm[k - 1]; cm[k - 1] = c;
            }
        }

        for (int i = 1; i < nn; i++) { // carry positions over; a new badge starts on its ring at a MAC angle
            int o = find(old, on, nd[i].mac);
            float a = (nd[i].mac[5] * 256 + nd[i].mac[4]) * 2 * (float)M_PI / 65536, d = cm[i] < 0 ? 1 : cm[i] / 100.0f;
            if (o >= 0) nd[i].x = old[o].x, nd[i].y = old[o].y;
            else nd[i].x = d * cosf(a), nd[i].y = d * sinf(a);
        }

        // Distances between any two listed badges: mine and shared, both directions averaged.
        memset(sum, 0, sizeof sum), memset(cnt, 0, sizeof cnt);
        for (int i = 0; i < n; i++) {
            int a = find(nd, nn, rg[i].a), b = find(nd, nn, rg[i].b);
            if (a < 0 || b < 0 || a == b || rg[i].cm < 0) continue;
            sum[a][b] += rg[i].cm / 100.0f, cnt[a][b]++;
            sum[b][a] += rg[i].cm / 100.0f, cnt[b][a]++;
        }
        for (int i = 0; i < nn; i++)
            for (int j = 0; j < nn; j++) dist[i][j] = cnt[i][j] ? sum[i][j] / cnt[i][j] : -1;

        // List: name with the distance on the line below; when those rows no longer fit, one compact line each.
        memset(fb, 0, sizeof fb);
        char s[40], id[13];
        bool fits = (nn - 1) * 56 <= H - TOP - 64; // 10 rows
        id_of(me, id);
        snprintf(s, sizeof s, "me %s", id);
        text(24, TOP + (fits ? 16 : 8), fits ? 3 : 2, s, true);
        for (int i = 1; i < nn; i++) {
            char d[12] = "...";
            if (cm[i] >= 0) snprintf(d, sizeof d, "%4.1f m", cm[i] / 100.0f);
            id_of(nd[i].mac, id);
            if (fits) {
                text(24, TOP + 8 + 56 * i, 3, id, true);
                text(72, TOP + 36 + 56 * i, 3, d, true);
            } else { // up to 31 rows of 19 px
                snprintf(s, sizeof s, "%-12s %s", id, d);
                text(24, TOP + 13 + 19 * i, 2, s, true);
            }
        }
        if (nn == 1) text(24, TOP + 96, 3, "Looking...", true);

        int cx = MAP_X + (W - MAP_X) / 2, cy = TOP + (H - TOP) / 2;
        if (measured >= 2) { // 3 badges with this one
            layout(cm, frame);
            float far = 0;
            for (int i = 1; i < nn; i++)
                if (cm[i] >= 0) far = fmaxf(far, hypotf(nd[i].x, nd[i].y));
            static const float steps[] = {0.5f, 1, 2, 5, 10, 20, 50};
            float step = 50; // ring spacing in metres: 3 rings cover the farthest badge
            for (int i = 0; i < 7; i++)
                if (3 * steps[i] >= far) { step = steps[i]; break; }
            float px = MAP_R / (3 * step);
            for (int k = 1; k <= 3; k++) {
                ring(cx, cy, MAP_R * k / 3);
                snprintf(s, sizeof s, "%g m", step * k);
                text(cx + 4, cy - MAP_R * k / 3 - 18, 2, s, true);
            }
            box(cx - 9, cy - 9, 18, 18, true);
            for (int i = 1; i < nn; i++) {
                if (cm[i] < 0) continue;
                int x = cx + snap(nd[i].x * px), y = cy + snap(nd[i].y * px);
                box(x - 7, y - 7, 14, 14, true);
                id_of(nd[i].mac, id);
                text(x + 12, y - 8, 2, id, true);
            }
            text(MAP_X, H - 24, 2, "rotation and mirror unknown", true);
        } else {
            text(MAP_X + 24, cy - 12, 3, "Map: needs 3+ badges", true);
        }
        ui_frame(1); // waits for the previous pull to end, then returns while this one runs. No change = no refresh

        int b = btn_get(FRAME_MS);
        if (b == BTN_A) ui_wipe();
        if (b == BTN_B) return; // the OS keeps ranging on every screen
    }
}

static void icon(int x, int y) // rings, a sweep line and two blips
{
    icon_base(x, y);
    int c = ICON / 2;
    for (int r = 0; r < ICON; r++)
        for (int k = 0; k < ICON; k++) {
            int d2 = (k - c) * (k - c) + (r - c) * (r - c);
            if ((d2 > 66 * 66 && d2 < 70 * 70) || (d2 > 42 * 42 && d2 < 46 * 46) || d2 < 6 * 6) box(x + k, y + r, 1, 1, false);
        }
    for (int t = 0; t < 48; t++) box(x + c + t, y + c - t, 4, 4, false); // sweep, to the top right
    box(x + c - 52, y + c - 20, 12, 12, false), box(x + c + 20, y + c + 36, 12, 12, false);
}

const app_t app_radar = {"Radar", icon, run};
