/* Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1> */
/*
 * fbui: the first-boot screens on the framebuffer. A dark page with the logo,
 * "Kali Linux for PS4" and the credits, plus one of:
 *
 *   fbui welcome LOGO [USB]       Install button; returns when any controller
 *                                 button / key / click is pressed (or after
 *                                 60 s, so a PS4 with no input still installs).
 *                                 With USB (a description of a USB drive):
 *                                 "Install to PS4" / "Install to USB drive"
 *                                 and an erase confirmation; exit code 10
 *                                 means the USB drive, 0 the PS4 drive.
 *                                 A Reboot button exits with code 20.
 *   fbui progress IMG FROM TO SIZE LABEL
 *                                 copies stdin to stdout and fills the bar from
 *                                 FROM% to TO% as SIZE bytes go through
 *   fbui msg IMG LABEL            just a message
 *   fbui busy IMG LABEL           full bar, a spinning arc and LABEL until
 *                                 killed (SIGTERM), for waits with no progress
 *   fbui done IMG LABEL           full bar and a final message
 *   fbui error IMG LABEL          message in red
 *   fbui text                     gives the screen back to the text console
 *
 * The logo is animated: embers fly in and form it, then an aura breathes
 * behind it and embers keep rising; during the install a ring around the
 * logo's disc fills with the progress (drawn from a separate thread, so the
 * data copy never waits for the screen). Up to 60 frames per second on the
 * intro and menus, 30 during the install.
 *
 * The console is put in graphics mode while the screens are up, so boot
 * messages do not draw over them. If there is no framebuffer, every mode
 * still works (progress still copies its data).
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font_data.h"

#define BG      0x0d1117
#define WHITE   0xf2f4f8
#define GREY    0x8b949e
#define ACCENT  0x367bf0
#define ACCENT2 0x2459b8
#define TRACK   0x232a35
#define RED     0xff6b6b

static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *fb;
static int fbfd = -1;
static unsigned char *frame;
static uint32_t *bb, *bb_static;
static int W, H;
static uint32_t aw, ah;
static unsigned char *avatar;

static void set_kd(int mode)
{
	int fd = open("/dev/tty0", O_RDWR);

	if (fd >= 0) {
		ioctl(fd, KDSETMODE, mode);
		close(fd);
	}
}

static void klog(const char *msg)
{
	int k = open("/dev/kmsg", O_WRONLY);

	if (k >= 0) {
		dprintf(k, "fbui: %s", msg);
		close(k);
	}
}

static void klogf(const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	klog(buf);
}

/* Every update rewrites the whole frame with one write(): on the PS4, small
 * updates through the DRM fbdev emulation can stay invisible until a large
 * write follows, so a full frame each time is what reliably reaches the TV. */
static int fb_open(void)
{
	char s[128];

	fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) < 0) {
		klog("no usable /dev/fb0");
		return -1;
	}
	snprintf(s, sizeof(s), "fb0 %ux%u %ubpp line %u yoffset %u", var.xres, var.yres,
		 var.bits_per_pixel, fix.line_length, var.yoffset);
	klog(s);
	if (var.bits_per_pixel != 32 && var.bits_per_pixel != 16)
		return -1;
	W = var.xres;
	H = var.yres;
	bb = malloc((size_t)W * H * 4);
	bb_static = malloc((size_t)W * H * 4);
	frame = calloc(H, fix.line_length);
	if (!bb || !bb_static || !frame)
		return -1;
	fb = frame;
	set_kd(KD_GRAPHICS);
	return 0;
}

static void blit(void)
{
	if (!fb)
		return;
	/* XRGB8888 (what amdgpu gives): the drawing buffer is already in the
	 * screen's format, so send it as-is (row by row only if padded). */
	if (var.bits_per_pixel == 32 && var.red.offset == 16 && var.green.offset == 8 &&
	    var.blue.offset == 0 && var.red.length == 8 && var.green.length == 8 &&
	    var.blue.length == 8) {
		if (fix.line_length == (unsigned)W * 4) {
			pwrite(fbfd, bb, (size_t)W * H * 4, (off_t)var.yoffset * fix.line_length);
			return;
		}
		for (int y = 0; y < H; y++)
			memcpy(frame + (size_t)y * fix.line_length, bb + (size_t)y * W, (size_t)W * 4);
		pwrite(fbfd, frame, (size_t)H * fix.line_length, (off_t)var.yoffset * fix.line_length);
		return;
	}
	for (int y = 0; y < H; y++) {
		unsigned char *row = frame + (size_t)y * fix.line_length;

		for (int x = 0; x < W; x++) {
			uint32_t c = bb[(size_t)y * W + x];
			unsigned r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;

			if (var.bits_per_pixel == 32)
				*(uint32_t *)(row + (size_t)x * 4) =
					(r >> (8 - var.red.length)) << var.red.offset |
					(g >> (8 - var.green.length)) << var.green.offset |
					(b >> (8 - var.blue.length)) << var.blue.offset;
			else
				*(uint16_t *)(row + (size_t)x * 2) =
					(uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
		}
	}
	pwrite(fbfd, frame, (size_t)H * fix.line_length, (off_t)var.yoffset * fix.line_length);
}

static void blend(int x, int y, uint32_t c, float a)
{
	if (x < 0 || y < 0 || x >= W || y >= H || a <= 0)
		return;
	if (a >= 1) {
		bb[(size_t)y * W + x] = c;
		return;
	}
	uint32_t d = bb[(size_t)y * W + x];
	unsigned r = (unsigned)((c >> 16 & 255) * a + (d >> 16 & 255) * (1 - a));
	unsigned g = (unsigned)((c >> 8 & 255) * a + (d >> 8 & 255) * (1 - a));
	unsigned b = (unsigned)((c & 255) * a + (d & 255) * (1 - a));

	bb[(size_t)y * W + x] = r << 16 | g << 8 | b;
}

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			blend(x, y, c, 1);
}

static float clampf(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
	unsigned r = (unsigned)((a >> 16 & 255) * (1 - t) + (b >> 16 & 255) * t);
	unsigned g = (unsigned)((a >> 8 & 255) * (1 - t) + (b >> 8 & 255) * t);
	unsigned bl = (unsigned)((a & 255) * (1 - t) + (b & 255) * t);

	return r << 16 | g << 8 | bl;
}

/* Anti-aliased rounded rectangle. */
static void rrect(int x0, int y0, int w, int h, int r, uint32_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++) {
			float cx = x < x0 + r ? x0 + r : x >= x0 + w - r ? x0 + w - r - 1 : x;
			float cy = y < y0 + r ? y0 + r : y >= y0 + h - r ? y0 + h - r - 1 : y;
			float d = hypotf(x - cx, y - cy);

			blend(x, y, c, clampf(r + 0.5f - d));
		}
}

static int text_w(const struct font *f, const char *s)
{
	int w = 0;

	for (; *s; s++)
		if (*s >= 32 && *s < 127)
			w += f->g[*s - 32].adv;
	return w;
}

/* Draws text with its top (ascent line) at y; x < 0 centres it. */
static void text(const struct font *f, int x, int y, const char *s, uint32_t c)
{
	if (x < 0)
		x = (W - text_w(f, s)) / 2;
	for (; *s; s++) {
		if (*s < 32 || *s >= 127)
			continue;
		const struct glyph *g = &f->g[*s - 32];

		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++)
				blend(x + g->xoff + i, y + g->yoff + j, c,
				      f->data[g->off + (size_t)j * g->w + i] / 255.0f);
		x += g->adv;
	}
}

static void load_avatar(const char *path)
{
	FILE *f = fopen(path, "rb");

	if (!f || fread(&aw, 4, 1, f) != 1 || fread(&ah, 4, 1, f) != 1 ||
	    !aw || !ah || aw > 2048 || ah > 2048 ||
	    !(avatar = malloc((size_t)aw * ah * 4)) ||
	    fread(avatar, 4, (size_t)aw * ah, f) != (size_t)aw * ah) {
		free(avatar);
		avatar = NULL;
	}
	if (f)
		fclose(f);
}

static int lower_top(void)
{
	return H * 64 / 100;
}

/* ---- Logo animation ------------------------------------------------------
 * Embers fly in from around the screen and form the logo, a light sweep
 * crosses it, then a purple aura breathes behind it while small embers keep
 * rising from it. Every frame is drawn into bb and sent with blit(). */
#define NEMB 1200
#define NSPOT 2048
#define NRISE 120
#define AURA_M 56

struct ember { float sx, sy, tx, ty, delay, dur, curve, r, g, b; };
struct riser { float x, y, vx, life, age, phase, r, g, b; };

static int lox, loy;			/* logo position */
static float disc_cx, disc_cy, disc_r;	/* the logo's magenta disc */
static float *aura;			/* soft glow mask around the logo */
static int ax0, ay0, aww, ahh;
static struct ember emb[NEMB];
static float emb_end;
static int spot_x[NSPOT], spot_y[NSPOT], nspot;
static struct riser rise[NRISE];
static float spr[7][7];
static uint32_t rng = 0x2545f491;

static float frand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng & 0xffffff) / 16777216.0f;
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static inline void addpx(int x, int y, float r, float g, float b)
{
	if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H)
		return;
	uint32_t *p = &bb[(size_t)y * W + x];
	int R = (int)(*p >> 16 & 255) + (int)r, G = (int)(*p >> 8 & 255) + (int)g;
	int B = (int)(*p & 255) + (int)b;

	*p = (uint32_t)(R > 255 ? 255 : R) << 16 | (uint32_t)(G > 255 ? 255 : G) << 8 |
	     (uint32_t)(B > 255 ? 255 : B);
}

/* A small glowing dot (additive). */
static void dot(float x, float y, float r, float g, float b, float k)
{
	int cx = (int)lrintf(x), cy = (int)lrintf(y);

	for (int j = -3; j <= 3; j++)
		for (int i = -3; i <= 3; i++) {
			float w = spr[j + 3][i + 3] * k;

			addpx(cx + i, cy + j, r * w, g * w, b * w);
		}
}

static const unsigned char *logo_px(int x, int y)
{
	return avatar + ((size_t)y * aw + x) * 4;
}

/* Box blur of a w*h float map, radius r, in place (one direction). */
static void box_pass(float *m, int w, int h, int r, int horiz)
{
	int n = horiz ? w : h, lines = horiz ? h : w;
	float *tmp = malloc(sizeof(float) * n);

	for (int l = 0; l < lines; l++) {
		float sum = 0;

		for (int i = 0; i < n; i++)
			tmp[i] = horiz ? m[(size_t)l * w + i] : m[(size_t)i * w + l];
		for (int i = -r; i <= r; i++)
			sum += tmp[i < 0 ? 0 : i >= n ? n - 1 : i];
		for (int i = 0; i < n; i++) {
			float v = sum / (2 * r + 1);
			int out = i - r, in = i + r + 1;

			if (horiz)
				m[(size_t)l * w + i] = v;
			else
				m[(size_t)i * w + l] = v;
			sum += tmp[in >= n ? n - 1 : in] - tmp[out < 0 ? 0 : out];
		}
	}
	free(tmp);
}

static void anim_init(void)
{
	int nopaque = 0, minx = 1 << 30, maxx = -1, miny = 1 << 30, maxy = -1;

	if (!avatar)
		return;
	lox = W / 2 - (int)aw / 2;
	loy = H * 31 / 100 - (int)ah / 2;

	for (int j = 0; j < 7; j++)
		for (int i = 0; i < 7; i++) {
			float d2 = (float)((i - 3) * (i - 3) + (j - 3) * (j - 3));

			spr[j][i] = expf(-d2 / 2.4f) + (d2 == 0 ? 0.4f : 0);
		}

	/* Opaque logo pixels: ember targets and rising-ember spawn points. */
	for (int y = 0; y < (int)ah; y++)
		for (int x = 0; x < (int)aw; x++) {
			const unsigned char *p = logo_px(x, y);

			if (p[3] > 128) {
				if (nspot < NSPOT) {
					spot_x[nspot] = x;
					spot_y[nspot++] = y;
				} else {
					int k = (int)(frand() * (nopaque + 1));

					if (k < NSPOT) {
						spot_x[k] = x;
						spot_y[k] = y;
					}
				}
				nopaque++;
			}
			if (p[3] > 230 && p[0] > 115 && p[2] > 115 && p[1] < 90) {
				minx = x < minx ? x : minx;
				maxx = x > maxx ? x : maxx;
				miny = y < miny ? y : miny;
				maxy = y > maxy ? y : maxy;
			}
		}
	if (maxx >= 0) {
		disc_cx = lox + (minx + maxx) / 2.0f;
		disc_cy = loy + (miny + maxy) / 2.0f;
		disc_r = fmaxf(maxx - minx, maxy - miny) / 2.0f;
	} else {
		disc_cx = W / 2.0f;
		disc_cy = H * 0.31f;
		disc_r = ah / 2.0f;
	}

	for (int i = 0; i < NEMB && nspot; i++) {
		struct ember *e = &emb[i];
		int k = (int)(frand() * nspot);
		const unsigned char *p = logo_px(spot_x[k], spot_y[k]);
		float ang = frand() * 6.2831853f, rad = 760 + frand() * 540;

		e->tx = lox + spot_x[k] + frand() - 0.5f;
		e->ty = loy + spot_y[k] + frand() - 0.5f;
		e->sx = W / 2.0f + rad * cosf(ang);
		e->sy = H * 0.31f + rad * sinf(ang) * 0.75f;
		e->delay = frand() * 0.7f;
		e->dur = 1.0f + frand() * 0.45f;
		e->curve = (frand() - 0.5f) * 360;
		e->r = fminf(255, p[0] * 1.3f + 25);
		e->g = fminf(255, p[1] * 1.3f + 25);
		e->b = fminf(255, p[2] * 1.3f + 25);
		emb_end = fmaxf(emb_end, e->delay + e->dur);
	}

	/* Aura: the logo's shape, widened and blurred. */
	ax0 = lox - AURA_M;
	ay0 = loy - AURA_M;
	aww = (int)aw + 2 * AURA_M;
	ahh = (int)ah + 2 * AURA_M;
	aura = calloc((size_t)aww * ahh, sizeof(float));
	if (!aura)
		return;
	for (int y = 0; y < (int)ah; y++)
		for (int x = 0; x < (int)aw; x++)
			aura[(size_t)(y + AURA_M) * aww + x + AURA_M] = logo_px(x, y)[3] / 255.0f;
	for (int pass = 0; pass < 3; pass++) {
		box_pass(aura, aww, ahh, 14, 1);
		box_pass(aura, aww, ahh, 14, 0);
	}
	for (size_t i = 0; i < (size_t)aww * ahh; i++)
		aura[i] = fminf(1, aura[i] * 1.8f);
}

static void draw_aura(float k)
{
	if (!aura || k <= 0)
		return;
	for (int y = 0; y < ahh; y++)
		for (int x = 0; x < aww; x++) {
			float a = aura[(size_t)y * aww + x] * k;

			if (a > 0.004f)
				addpx(ax0 + x, ay0 + y, 184 * a, 40 * a, 220 * a);
		}
}

static void draw_logo(float alpha)
{
	for (int y = 0; y < (int)ah; y++)
		for (int x = 0; x < (int)aw; x++) {
			const unsigned char *p = logo_px(x, y);

			if (p[3])
				blend(lox + x, loy + y, (uint32_t)p[0] << 16 | p[1] << 8 | p[2],
				      p[3] / 255.0f * alpha);
		}
}

/* A diagonal band of light across the logo, centred at `pos`. */
static void draw_sweep(float pos)
{
	for (int y = 0; y < (int)ah; y++)
		for (int x = 0; x < (int)aw; x++) {
			const unsigned char *p = logo_px(x, y);
			float d = lox + x + 0.45f * y - pos;

			if (p[3] && fabsf(d) < 40) {
				float k = expf(-d * d / 200) * p[3] / 255.0f * 150;

				addpx(lox + x, loy + y, k, k, k);
			}
		}
}

/* Embers rising from the logo; dt is the time since the previous frame. */
static void risers(float dt, float rate)
{
	static float acc;

	acc += rate * dt;
	for (int i = 0; i < NRISE && acc >= 1; i++) {
		struct riser *r = &rise[i];

		if (r->life > 0)
			continue;
		int k = (int)(frand() * nspot);

		r->x = lox + spot_x[k];
		r->y = loy + spot_y[k];
		r->vx = (frand() - 0.5f) * 24;
		r->life = 1.4f + frand() * 1.2f;
		r->age = 0;
		r->phase = frand() * 6.28f;
		if (frand() < 0.6f) {
			r->r = 255; r->g = 140; r->b = 50;
		} else {
			r->r = 215; r->g = 80; r->b = 240;
		}
		acc -= 1;
	}
	if (acc > 3)
		acc = 3;
	for (int i = 0; i < NRISE; i++) {
		struct riser *r = &rise[i];

		if (r->life <= 0)
			continue;
		r->age += dt;
		if (r->age >= r->life) {
			r->life = 0;
			continue;
		}
		r->x += (r->vx + 16 * sinf(r->age * 3 + r->phase)) * dt;
		r->y -= 56 * dt;
		dot(r->x, r->y, r->r, r->g, r->b, (1 - r->age / r->life) * (0.6f + 0.4f * frand()) * 0.9f);
	}
}

/* Progress ring around the logo's disc: purple to blue, glowing head. */
static void draw_ring(float pct, float pulse)
{
	float r = disc_r + 20, ext = 16;
	int x0 = (int)(disc_cx - r - ext), x1 = (int)(disc_cx + r + ext);
	int y0 = (int)(disc_cy - r - ext), y1 = (int)(disc_cy + r + ext);

	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			float dx = x + 0.5f - disc_cx, dy = y + 0.5f - disc_cy;
			float d = sqrtf(dx * dx + dy * dy) - r;

			if (fabsf(d) > ext)
				continue;
			float f = atan2f(dx, -dy) / 6.2831853f;

			if (f < 0)
				f += 1;
			float band = clampf(2.2f - fabsf(d));

			if (f <= pct) {
				uint32_t c = mix(0xb829dc, 0x367bf0, f);
				float g = expf(-d * d / 50) * (0.35f + 0.2f * pulse);

				addpx(x, y, (c >> 16 & 255) * g, (c >> 8 & 255) * g, (c & 255) * g);
				blend(x, y, c, band);
			} else {
				blend(x, y, TRACK, band);
			}
		}
	if (pct > 0) {
		float a = pct * 6.2831853f;
		float hx = disc_cx + r * sinf(a), hy = disc_cy - r * cosf(a);

		dot(hx, hy, 200, 215, 255, 1.2f);
		dot(hx, hy, 120, 140, 255, 0.8f);
	}
}

/* A glowing arc circling the disc, for waits with no measurable progress. */
static void draw_spinner(float phase)
{
	const float len = 0.28f;
	float r = disc_r + 20, ext = 16;
	int x0 = (int)(disc_cx - r - ext), x1 = (int)(disc_cx + r + ext);
	int y0 = (int)(disc_cy - r - ext), y1 = (int)(disc_cy + r + ext);

	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			float dx = x + 0.5f - disc_cx, dy = y + 0.5f - disc_cy;
			float d = sqrtf(dx * dx + dy * dy) - r;

			if (fabsf(d) > ext)
				continue;
			float f = atan2f(dx, -dy) / 6.2831853f - phase;

			f -= floorf(f);
			float band = clampf(2.2f - fabsf(d));

			if (f <= len) {
				float q = f / len;	/* 0 at the tail, 1 at the head */
				uint32_t c = mix(0xb829dc, 0x367bf0, q);
				float g = expf(-d * d / 50) * 0.55f * q;

				addpx(x, y, (c >> 16 & 255) * g, (c >> 8 & 255) * g, (c & 255) * g);
				blend(x, y, c, band * (0.25f + 0.75f * q));
			} else {
				blend(x, y, TRACK, band);
			}
		}
	float a = (phase + len) * 6.2831853f;

	dot(disc_cx + r * sinf(a), disc_cy - r * cosf(a), 200, 215, 255, 1.2f);
}

static int spinning;

/* The animated logo for one frame: aura, optional ring, logo, rising embers. */
static void scene(float ring_pct)
{
	static double last;
	double t = now_s();
	float dt = last > 0 ? (float)(t - last) : 0;

	last = t;
	if (dt > 0.25f)
		dt = 0.25f;
	if (!avatar)
		return;
	draw_aura(0.28f + 0.22f * (0.5f + 0.5f * sinf((float)fmod(t, 1000.0) * 3.14159f)));
	if (spinning)
		draw_spinner((float)fmod(t * 0.9, 1.0));
	else if (ring_pct >= 0)
		draw_ring(ring_pct, 0.5f + 0.5f * sinf((float)fmod(t, 1000.0) * 4));
	draw_logo(1);
	risers(dt, 22);
}

static void fill_bg(void)
{
	for (size_t i = 0; i < (size_t)W * H; i++)
		bb[i] = BG;
}

static int next_action(int ms);

/* Intro: embers form the logo. Any button skips it. */
static void intro(void)
{
	double t0 = now_s(), last = t0;

	if (!fb || !avatar || !nspot)
		return;
	for (;;) {
		double tn = now_s();
		float t = (float)(tn - t0), dt = (float)(tn - last);

		last = tn;
		if (t > emb_end + 0.75f || next_action(0) != 0)
			break;
		fill_bg();
		draw_aura(clampf((t - emb_end + 0.2f) / 0.6f) * 0.5f);
		float la = clampf((t - emb_end + 0.35f) / 0.45f);

		if (la > 0)
			draw_logo(la);
		float fade = 1 - clampf((t - emb_end + 0.1f) / 0.5f);

		for (int i = 0; i < NEMB && fade > 0; i++) {
			struct ember *e = &emb[i];

			if (t < e->delay)
				continue;
			float u = clampf((t - e->delay) / e->dur), q = 1 - u;
			float ez = 1 - q * q * q;
			float dx = e->tx - e->sx, dy = e->ty - e->sy, len = sqrtf(dx * dx + dy * dy) + 1e-3f;
			float bend = sinf(3.14159f * u) * e->curve;
			float x = e->sx + dx * ez - dy / len * bend;
			float y = e->sy + dy * ez + dx / len * bend;
			float r = 255 * (1 - ez) + e->r * ez, g = 140 * (1 - ez) + e->g * ez;
			float b = 50 * (1 - ez) + e->b * ez;

			dot(x, y, r, g, b, fade * (0.75f + 0.25f * frand()) * 0.9f);
		}
		float sw = (t - emb_end - 0.05f) / 0.6f;

		if (sw >= 0 && sw <= 1)
			draw_sweep(lox - 120 + (aw + 240) * sw);
		if (t > emb_end - 0.2f)
			risers(dt, 22);
		blit();
		double used = now_s() - tn;	/* at most 60 frames per second */

		if (used < 0.016)
			usleep((useconds_t)((0.016 - used) * 1e6));
	}
}
/* -------------------------------------------------------------------------- */

/* Background, title and credits (the logo itself is drawn by scene()). */
static void header(void)
{
	int cy = H * 31 / 100, top;

	fill_bg();
	top = avatar ? cy - (int)ah / 2 + (int)ah + 36 : cy;
	text(&font_title, -1, top, "Kali Linux for PS4", WHITE);
	text(&font_body, -1, top + 64, "Credits: blue0x1", GREY);
}

static void clear_lower(void)
{
	fill(0, lower_top() - 16, W, H - lower_top() + 16, BG);
}

static const int BTN_H = 72;

/* Signed distance from a pixel centre to a rounded rectangle (negative inside). */
static float rr_dist(float px, float py, int x0, int y0, int w, int h, int r)
{
	float qx = fabsf(px - (x0 + w / 2.0f)) - (w / 2.0f - r);
	float qy = fabsf(py - (y0 + h / 2.0f)) - (h / 2.0f - r);

	return hypotf(qx > 0 ? qx : 0, qy > 0 ? qy : 0) + fminf(fmaxf(qx, qy), 0) - r;
}

#define GLOW 22

/* Pill button: vertical gradient top->bot, a light sheen on the upper half,
 * and an optional soft glow around it. */
static void pill(int x, int y, int w, int h, uint32_t top, uint32_t bot, uint32_t glow, float glow_a)
{
	int r = h / 2;

	for (int yy = y - GLOW; yy < y + h + GLOW; yy++)
		for (int xx = x - GLOW; xx < x + w + GLOW; xx++) {
			float d = rr_dist(xx + 0.5f, yy + 0.5f, x, y, w, h, r);

			if (glow_a > 0 && d > 0 && d < GLOW) {
				float t = 1 - d / GLOW;

				blend(xx, yy, glow, glow_a * t * t);
			}
			if (d < 0.5f) {
				float t = (yy - y) / (float)(h - 1);
				uint32_t c = mix(top, bot, t < 0 ? 0 : t > 1 ? 1 : t);

				if (t < 0.45f)
					c = mix(c, 0xffffff, 0.10f * (1 - t / 0.45f));
				blend(xx, yy, c, clampf(0.5f - d));
			}
		}
}

struct btn_style { uint32_t top, bot, glow, dtop, dbot, ftop, fbot, ftext; };

static const struct btn_style STYLE_BLUE = {
	0x5a9cff, 0x2563eb, 0x3b82f6,		/* selected */
	0x3f7fe0, 0x1d4ed8,			/* pressed */
	0x2a3549, 0x1d2533, 0xaab8d0,		/* not selected */
};
static const struct btn_style STYLE_RED = {
	0xff6b6b, 0xd63031, 0xff4d4f,
	0xe05555, 0xb02525,
	0x40252b, 0x2c1a1f, 0xff9a9a,
};

/* Reboot and erasing are shown in red, everything else in blue. */
static const struct btn_style *style_for(const char *label)
{
	return !strcmp(label, "Reboot") || !strcmp(label, "Erase and install") ? &STYLE_RED : &STYLE_BLUE;
}

/* A row of 1-3 buttons; `sel` is highlighted with a glow, `down` draws it pressed. */
static void buttons(const char *const *l, int n, int sel, int down, int by)
{
	int bw = n == 1 ? 300 : n == 2 ? 400 : 360, gap = n == 3 ? 32 : 40;
	int x0 = (W - n * bw - (n - 1) * gap) / 2;

	fill(0, by - GLOW, W, BTN_H + 2 * GLOW, BG);
	for (int i = 0; i < n; i++) {
		const struct btn_style *st = style_for(l[i]);
		int x = x0 + i * (bw + gap);

		if (i != sel)
			pill(x, by, bw, BTN_H, st->ftop, st->fbot, 0, 0);
		else if (down)
			pill(x, by, bw, BTN_H, st->dtop, st->dbot, st->glow, 0.35f);
		else
			pill(x, by, bw, BTN_H, st->top, st->bot, st->glow, 0.55f);
		text(&font_bold, x + (bw - text_w(&font_bold, l[i])) / 2,
		     by + (BTN_H - font_bold.height) / 2 + 2, l[i], i == sel ? WHITE : st->ftext);
	}
}

enum { A_NONE, A_LEFT, A_RIGHT, A_OK, A_BACK, A_OTHER };

struct dev { int fd, stick, sx; char name[32]; };
static struct dev devs[16];
static int ndevs;

static void scan_inputs(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;

	while (d && (e = readdir(d)) && ndevs < 16) {
		struct input_absinfo ai;
		char path[300];
		int known = 0, fd;

		if (strncmp(e->d_name, "event", 5))
			continue;
		for (int i = 0; i < ndevs; i++)
			known |= !strcmp(devs[i].name, e->d_name);
		if (known)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(&devs[ndevs], 0, sizeof(devs[ndevs]));
		devs[ndevs].fd = fd;
		/* A gamepad stick reports 0..255; touchpads and motion sensors
		 * use much larger ranges and must not move the selection. */
		devs[ndevs].stick = ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum == 255;
		snprintf(devs[ndevs].name, sizeof(devs[ndevs].name), "%.31s", e->d_name);
		{
			char dn[64] = "?";

			ioctl(fd, EVIOCGNAME(sizeof(dn)), dn);
			klogf("input %s: %s", e->d_name, dn);
		}
		ndevs++;
	}
	if (d)
		closedir(d);
}

static int action(struct dev *dv, const struct input_event *ev)
{
	if (ev->type == EV_KEY && ev->value == 1) {
		switch (ev->code) {
		case KEY_LEFT: case BTN_DPAD_LEFT: return A_LEFT;
		case KEY_RIGHT: case BTN_DPAD_RIGHT: return A_RIGHT;
		case BTN_SOUTH: case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
		case BTN_LEFT: case BTN_START: return A_OK;
		case BTN_EAST: case KEY_ESC: case KEY_BACKSPACE: return A_BACK;
		/* The same pad seen as a generic HID joystick (no Sony driver):
		 * Cross is BTN_THUMB and Circle BTN_THUMB2. */
		case BTN_THUMB: return A_OK;
		case BTN_THUMB2: return A_BACK;
		case KEY_POWER: case KEY_SLEEP: case KEY_WAKEUP: return A_NONE;
		default: return A_OTHER;
		}
	}
	if (ev->type == EV_ABS && ev->code == ABS_HAT0X && ev->value)
		return ev->value < 0 ? A_LEFT : A_RIGHT;
	if (ev->type == EV_ABS && ev->code == ABS_X && dv->stick) {
		int dir = ev->value < 40 ? -1 : ev->value > 215 ? 1 : 0, was = dv->sx;

		dv->sx = dir;
		if (dir && dir != was)
			return dir < 0 ? A_LEFT : A_RIGHT;
	}
	return A_NONE;
}

static int next_action(int ms)
{
	static time_t last_scan;
	struct pollfd fds[16];

	if (time(NULL) != last_scan) {
		scan_inputs();
		last_scan = time(NULL);
	}
	for (int i = 0; i < ndevs; i++) {
		fds[i].fd = devs[i].fd;
		fds[i].events = POLLIN;
	}
	if (poll(fds, ndevs, ms) <= 0) {
		if (ndevs == 0 && ms > 0)
			usleep(ms * 1000);
		return A_NONE;
	}
	for (int i = 0; i < ndevs; i++) {
		struct input_event ev;

		while (fds[i].revents & POLLIN && read(devs[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
			int a = action(&devs[i], &ev);

			if (a != A_NONE)
				return a;
		}
	}
	return A_NONE;
}

/*
 * Shows `title` (optional), a row of `n` buttons, and up to two hint lines. Returns the chosen button (0 or 1), `back` when Circle/Esc
 * is pressed (ignored if `back` < 0), or -1 when `timeout` seconds pass with
 * no input at all. Any input stops the countdown. With one button, any key or
 * controller button chooses it.
 */
static int choose(const char *title, uint32_t tcolor, const char *const *l, int n, int sel,
		  int back, int timeout, const char *hint, const char *hint2)
{
	int ty = lower_top() - 30, by = lower_top() + 30;
	int hy = by + BTN_H + 34, cy = hy + 36;
	int left = timeout, counting = timeout > 0, dirty = 1, down = 0;
	time_t start = time(NULL);

	for (;;) {
		double frame_start = now_s();

		if (fb) {
			if (dirty) {
				header();
				clear_lower();
				if (title)
					text(&font_body, -1, ty, title, tcolor);
				if (hint)
					text(&font_small, -1, hy, hint, GREY);
				if (counting) {
					char s[96];

					snprintf(s, sizeof(s), "%s %d s",
						 hint2 ? hint2 : "Installs automatically in", left > 0 ? left : 0);
					text(&font_small, -1, cy, s, GREY);
				}
				buttons(l, n, sel, down, by);
				memcpy(bb_static, bb, (size_t)W * H * 4);
				dirty = 0;
			}
			memcpy(bb, bb_static, (size_t)W * H * 4);
			scene(-1);
			blit();
		}
		if (down) {
			usleep(150000);
			return sel;
		}
		/* Up to 60 frames per second: wait for input only for what is
		 * left of this frame's 16 ms. */
		int wait = fb ? 16 - (int)((now_s() - frame_start) * 1000) : 200;
		int act = next_action(wait < 1 ? 1 : wait);

		if (act == A_NONE) {
			if (counting) {
				int now = timeout - (int)(time(NULL) - start);

				if (now <= 0)
					return -1;
				if (now != left) {
					left = now;
					dirty = 1;
				}
			}
			continue;
		}
		if (counting) {
			counting = 0;
			dirty = 1;
		}
		if (act == A_LEFT && sel > 0) {
			sel--;
			dirty = 1;
		} else if (act == A_RIGHT && sel < n - 1) {
			sel++;
			dirty = 1;
		} else if (act == A_BACK && back >= 0) {
			return back;
		} else if (act == A_OK || (n == 1 && act == A_OTHER)) {
			if (!fb)
				return sel;
			down = 1;
			dirty = 1;
		}
	}
}

/* Exit code 0: install to the PS4 drive; 10: install to the USB drive;
 * 20: reboot without installing. */
static int welcome(const char *usb)
{
	static const char *const plain[] = { "Install", "Reboot" };
	static const char *const withusb[] = { "Install to PS4", "Install to USB drive", "Reboot" };
	static const char *const confirm[] = { "Back", "Erase and install" };
	char info[200], warn[200];

	intro();
	if (!usb || !*usb)
		return choose(NULL, WHITE, plain, 2, 0, -1, 60,
			      "Left / Right to choose, X to confirm", NULL) == 1 ? 20 : 0;
	snprintf(info, sizeof(info), "USB drive: %s", usb);
	for (;;) {
		int r = choose("Where do you want to install Kali?", WHITE, withusb, 3, 0, -1, 60, info,
			       "Installs to the PS4 automatically in");

		if (r == 2)
			return 20;
		if (r != 1)
			return 0;
		snprintf(warn, sizeof(warn), "Everything on the USB drive will be erased (%s)", usb);
		if (choose(warn, RED, confirm, 2, 0, 0, 0,
			   "Left / Right to choose, X to confirm, Circle to go back", NULL) == 1)
			return 10;
	}
}

static int bar_x(void) { return (W - 760) / 2; }

static void bar(int permille)
{
	int x = bar_x(), y = lower_top() + 60, w = 760, h = 14;
	int fw = w * permille / 1000;
	char s[64];

	fill(0, y - 4, W, 90, BG);
	rrect(x, y, w, h, h / 2, TRACK);
	if (fw >= h)
		rrect(x, y, fw, h, h / 2, ACCENT);
	snprintf(s, sizeof(s), "%d%%", permille / 10);
	text(&font_small, -1, y + 34, s, GREY);
}

/* Static part of a message/progress page into bb_static. */
static void page_static(const char *label, uint32_t color, const char *hint)
{
	header();
	clear_lower();
	text(&font_body, -1, lower_top(), label, color);
	if (hint)
		text(&font_small, -1, lower_top() + 150, hint, GREY);
	memcpy(bb_static, bb, (size_t)W * H * 4);
}

static void page(const char *label, uint32_t color, int permille, const char *hint)
{
	if (!fb)
		return;
	page_static(label, color, hint);
	if (permille >= 0)
		bar(permille);
	scene(permille >= 0 ? permille / 1000.0f : -1);
	blit();
}

static unsigned long long prog_done;
static int prog_end;
static int prog_from, prog_to;
static unsigned long long prog_size;

static int prog_permille(void)
{
	unsigned long long d = __atomic_load_n(&prog_done, __ATOMIC_RELAXED);

	if (d > prog_size)
		d = prog_size;
	return prog_from * 10 + (int)((prog_to - prog_from) * 10ULL * d / (prog_size ? prog_size : 1));
}

/* Redraws the progress page about 8 times a second until the copy ends. */
static void *prog_draw(void *unused)
{
	(void)unused;
	for (;;) {
		double t0 = now_s();
		int end = __atomic_load_n(&prog_end, __ATOMIC_ACQUIRE);
		int pm = prog_permille();

		memcpy(bb, bb_static, (size_t)W * H * 4);
		bar(pm);
		scene(pm / 1000.0f);
		blit();
		if (end)
			return NULL;
		double used = now_s() - t0;	/* about 30 frames per second */

		if (used < 0.033)
			usleep((useconds_t)((0.033 - used) * 1e6));
	}
}

static int progress(int from, int to, unsigned long long size, const char *label)
{
	static char buf[1 << 20];
	unsigned long long done = 0, since_sync = 0;
	struct stat st;
	int regular = fstat(1, &st) == 0 && S_ISREG(st.st_mode), drawing = 0, ret = 0;
	pthread_t th;
	ssize_t r;

	prog_from = from;
	prog_to = to;
	prog_size = size;
	if (fb) {
		page_static(label, WHITE, "Please do not turn off your PS4");
		drawing = pthread_create(&th, NULL, prog_draw, NULL) == 0;
	}
	while ((r = read(0, buf, sizeof(buf))) > 0) {
		for (ssize_t off = 0; off < r;) {
			ssize_t w = write(1, buf + off, (size_t)(r - off));

			if (w <= 0) {
				ret = 1;
				goto out;
			}
			off += w;
		}
		done += (unsigned long long)r;
		since_sync += (unsigned long long)r;
		/* Keep the page cache small so the bar follows the real writes. */
		if (regular && since_sync >= (64ULL << 20)) {
			fdatasync(1);
			since_sync = 0;
		}
		__atomic_store_n(&prog_done, done, __ATOMIC_RELAXED);
	}
	if (regular)
		fdatasync(1);
	ret = r < 0;
out:
	if (drawing) {
		__atomic_store_n(&prog_end, 1, __ATOMIC_RELEASE);
		pthread_join(th, NULL);
	}
	return ret;
}

static volatile sig_atomic_t stop_busy;

static void on_term(int sig)
{
	(void)sig;
	stop_busy = 1;
}

/* Animated "still working" page until SIGTERM. */
static void busy(const char *label)
{
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	if (!fb) {
		while (!stop_busy)
			pause();
		return;
	}
	page_static(label, WHITE, "Please do not turn off your PS4");
	bar(1000);
	memcpy(bb_static, bb, (size_t)W * H * 4);
	spinning = 1;
	while (!stop_busy) {
		double t0 = now_s();

		memcpy(bb, bb_static, (size_t)W * H * 4);
		scene(-1);
		blit();
		double used = now_s() - t0;	/* about 30 frames per second */

		if (used < 0.033)
			usleep((useconds_t)((0.033 - used) * 1e6));
	}
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";

	if (!strcmp(mode, "text")) {
		set_kd(KD_TEXT);
		return 0;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: fbui welcome|progress|done|error|text IMG ...\n");
		return 2;
	}
	load_avatar(argv[2]);
	if (fb_open() < 0)
		fb = NULL;
	if (fb)
		anim_init();

	if (!strcmp(mode, "welcome")) {
		return welcome(argc > 3 ? argv[3] : NULL);
	} else if (!strcmp(mode, "progress") && argc == 7) {
		return progress(atoi(argv[3]), atoi(argv[4]), strtoull(argv[5], NULL, 10), argv[6]);
	} else if (!strcmp(mode, "busy") && argc == 4) {
		busy(argv[3]);
	} else if (!strcmp(mode, "msg") && argc == 4) {
		page(argv[3], WHITE, -1, NULL);
	} else if (!strcmp(mode, "done") && argc == 4) {
		page(argv[3], WHITE, 1000, NULL);
	} else if (!strcmp(mode, "error") && argc == 4) {
		page(argv[3], RED, -1, "A text console with the details opens in a few seconds");
	} else {
		fprintf(stderr, "fbui: bad arguments\n");
		return 2;
	}
	return 0;
}
