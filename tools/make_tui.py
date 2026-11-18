#!/usr/bin/env python3
"""OTTmpc's native MPC skin: skins/TUI.json + skins/Q-Links.json + the PNGs they use (after Profit-5's / Welle's make_tui).

One page, the 1.x look (vst/art/make_art.py's panels, knob caps and scales, the 24-step band meters), redrawn for the
native widgets: the knob caps turned into 128-frame film strips on their scales, captions baked in (Titillium Web, OFL), the
values as live labels in the wells. Q-Links in three blocks of four that do not overlap:
    A  DEPTH  TIME  IN GAIN  OUT GAIN          left panel, 2 x 2 (as 1.x)
    B  H  M  L  MIX                            the band column and MIX beside it
    C  UPWARD  DOWNWARD  LOW/MID  MID/HIGH     the right column
SAVE PRESET / DELETE PRESET sit under the left block; SAVE PRESET opens the drawn naming keyboard (kb_open = 1).
    python3 tools/make_tui.py [--preview]     # -> skins/  (build.sh runs it; --preview also writes build/skin-preview/*.png)"""
import json, os, re, shutil, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("OTT_SKIN_OUT") or os.path.join(ROOT, "skins")
PICTURE_METERS = os.environ.get("OTT_PICTURE_METERS") == "1"   # 1.x's 24-step picture bars instead of the native Meter widget
ART = os.path.join(ROOT, "vst", "art")
FONTS = os.path.join(ROOT, "tools", "fonts")
PREVIEW = os.path.join(ROOT, "build", "skin-preview")
LOCK = [l.strip() for l in open(os.path.join(ROOT, "params.lock")) if l.strip() and not l.startswith("#")]
PARAMS = {p["key"]: p for p in json.load(open(os.path.join(ROOT, "vst", "params.json")))["params"]}
GEN = open(os.path.join(ROOT, "src", "params_gen.h")).read()
FAV_NAME_MAX = int(re.search(r"kFavNameMax = (\d+)", GEN).group(1))   # the limit the plugin enforces
FRAMES = 128               # knob strips
STEPS = 24                 # meter steps (src/engine.cpp METER_STEPS)
FONT = "Titillium Web"
GESTURE = "Instant"
DEVICE_TEXT = 0.64         # the MPC draws a Label's font at about this fraction of its height (measured on the device)
ACCENT = "ffe8eef0"        # layout.conf theme_accent

sys.path.insert(0, ART)
import make_art as ma      # noqa: E402  (the 1.x art: colours, gradients, knob caps)

INK, INK_LIGHT, INK_TEAL = (34, 39, 44), (232, 238, 240), (143, 176, 170)
WELL, WELL_TEAL = (44, 50, 56), (12, 30, 34)


def P(key):
    return "Parameter %d" % LOCK.index(key)


def ttf(px, weight="SemiBold"):
    return ImageFont.truetype(os.path.join(FONTS, "TitilliumWeb-%s.ttf" % weight), int(round(px)))


# ---- a JSON writer that keeps key order and allows the overlay widget's duplicate "version" key ------------------------
class Obj:
    def __init__(self, *pairs):
        self.pairs = list(pairs)


def dump(v, ind=0):
    sp = "  " * ind
    pairs = v.pairs if hasattr(v, "pairs") else list(v.items()) if isinstance(v, dict) else None
    if pairs is not None:
        if not pairs:
            return "{}"
        return "{\n%s\n%s}" % (",\n".join("%s  %s: %s" % (sp, dump(k), dump(x, ind + 1)) for k, x in pairs), sp)
    if isinstance(v, list):
        return "[]" if not v else "[\n%s\n%s]" % (",\n".join(sp + "  " + dump(x, ind + 1) for x in v), sp)
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v) if isinstance(v, float) else str(v)
    return '"%s"' % str(v).replace("\\", "\\\\").replace('"', '\\"')


# ---- building blocks ---------------------------------------------------------------------------------------------------
def B(x, y, w, h):
    return "%d %d %d %d" % (x, y, w, h)


def entry(name, typ, data, b, remap=None, focus="No", ind=None):
    bd = {"version": 2 if ind else 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": "Show", "whenVisible": "Always",
          "boundsType": "Absolute", "bounds": b}
    if ind:   # shown only while the handle has that index (needs bounds version 2)
        bd["additionalInvalidatingHandles"] = ind
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": [{"key": "Data", "value": remap}] if remap else []},
            "bounds": bd}


def bg(colour="0"):
    return {"version": 1, "focussed": {"version": 1, "colour": colour, "image": ""}, "unfocussed": {"version": 1, "colour": colour, "image": ""}}


def component(children, actions=None, ignore=False, background=None):
    return {"version": 2, "actions": actions or [], "backgroundData": background or bg(), "ignoreMousePresses": ignore,
            "disableCoarseDataWheel": False, "componentsData": children}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


FOCUS = {"version": 1, "backgroundColour": "00000000", "outlineColour": ACCENT, "backgroundInset": 4.0, "outlineThickness": 2.0}
QL = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "knob overlay"), act("Enter Pressed", "Show Overlay", "knob overlay")]


def focus_ring(b):
    e = entry("focus", "Focus", FOCUS, b)
    e["bounds"]["whenVisible"] = "WhenFocussed"
    return e


def text_style(h, colour, style="SemiBold", just="horizontallyCentred verticallyCentred"):
    return {"version": 1, "font": {"version": 1, "name": FONT, "style": style, "height": h}, "colour": colour, "justification": just,
            "case": "Original"}


def label(b, remap, h=24.0, colour="ffe8eef0", style="SemiBold", just="horizontallyCentred verticallyCentred", ind=None):
    return entry("label", "Label", {"version": 1, "textStyle": text_style(h, colour, style, just), "type": "Value", "handleName": "Data"},
                 b, remap, ind=ind)


def overlay_def():
    return component([entry("overlay", "KnobOverlay", Obj(
        ("version", 2), ("version", 1),
        ("actions", [{"version": 1, "onAction": "Focus Lost", "handler": "Focus Lost", "handleName": "Focus Lost", "additionalData": ""}]),
        ("overlayType", "Knob With Header"), ("scaleType", "Linear"), ("polarityType", "Unipolar"),
        ("showMinusSign", True), ("handleName", "Data")), "0 0 1280 800", focus="Yes")], ignore=True)


DEFS = {}


def knob_def(strip, size):
    """A film-strip knob (dragged vertically), a transparent focus ring, Q-Link on touch, the overlay on double tap / Enter."""
    key = "knob_" + os.path.splitext(strip)[0]
    if key not in DEFS:
        DEFS[key] = component([
            focus_ring(B(-4, -4, size + 8, size + 8)),
            entry("strip", "Knob", {"version": 3, "knobType": "FilmStrip", "filmStrip": strip, "numFrames": FRAMES, "invert": False,
                                    "dragOrientation": "Vertical", "handleName": "Data"}, B(0, 0, size, size)),
        ], actions=QL)
    return key


def save(im, name):
    im.save(os.path.join(OUT, name))
    return name


# ---- art ----------------------------------------------------------------------------------------------------------------
def knob_strip(cap, scale, size, name):
    """128 frames: the 1.x scale with the 1.x cap turned from -135 to +135 degrees (its pointer is up at the middle)."""
    sc = Image.open(os.path.join(ART, scale)).convert("RGBA")
    cp = Image.open(os.path.join(ART, cap)).convert("RGBA")
    strip = Image.new("RGBA", (size, size * FRAMES), (0, 0, 0, 0))
    for f in range(FRAMES):
        fr = sc.copy()
        rot = cp.rotate(135.0 - 270.0 * f / (FRAMES - 1), resample=Image.BICUBIC)
        fr.alpha_composite(rot, ((sc.width - cp.width) // 2, (sc.height - cp.height) // 2))
        strip.paste(fr.resize((size, size), Image.LANCZOS) if size != sc.width else fr, (0, f * size))
    assert strip.height <= 16384, "film strip too tall for the MPC"
    return save(strip, name)


def meter_strip(prefix, name):
    """The 1.x meter pictures (vst/art/meter/<prefix>_00..23.png) as one vertical strip."""
    frames = [Image.open(os.path.join(ART, "meter", "%s_%02d.png" % (prefix, i))).convert("RGB") for i in range(STEPS)]
    w, h = frames[0].size
    strip = Image.new("RGB", (w, h * STEPS))
    for i, im in enumerate(frames):
        strip.paste(im, (0, i * h))
    return save(strip, name)


def meter_images(band):
    """The native Meter's two pictures of one band's output bar, at the size it is drawn (the widget does not scale them): the
    threshold zones unlit (1.x's bar at step 0 without its cursor) and lit. The widget reveals the lit one up to h_level and draws
    the peak-hold line at h_hold."""
    lo, hi = ma.THRESH[band]
    xl, xh = ma.x_of(lo), ma.x_of(hi)
    out = []
    for lit in (False, True):
        im = Image.new("RGB", (ma.BAR_W, ma.BAR_H), ma.GAP)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, xl - 1, ma.BAR_H], fill=ma.BEIGE if lit else ma.BEIGE_DIM)
        if lit:
            d.rectangle([xl, 0, xh - 1, ma.BAR_H], fill=(58, 92, 88))   # between the thresholds: neither up nor down
        d.rectangle([xh, 0, ma.BAR_W, ma.BAR_H], fill=ma.GREEN if lit else ma.GREEN_DIM)
        out.append(save(im, "meter_%s_%s.png" % (band, "lit" if lit else "off")))
    return out


def key_art(w, h, caption, lit):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w - 1, h - 1), 8, fill=(30, 66, 72) if lit else WELL, outline=(232, 238, 240) if lit else (20, 24, 28), width=2)
    d.text((w / 2, h / 2 + 1), caption, font=ttf(17, "Bold"), fill=(255, 255, 255) if lit else INK_LIGHT, anchor="mm")
    return im


# ---- the page ------------------------------------------------------------------------------------------------------------
# (key, caption, cx, cy, size of the knob picture, well (x, y, w, h), well colour) in screen coordinates (content area 1280 x 628)
BIG, BAND, SMALL = 120, 92, 102
KNOBS = [
    ("depth", "DEPTH", 110, 232, BIG), ("time", "TIME", 290, 232, BIG),
    ("in_gain", "IN GAIN", 110, 452, BIG), ("out_gain", "OUT GAIN", 290, 452, BIG),
    ("h_gain", "H", 948, 118, BAND), ("m_gain", "M", 948, 288, BAND), ("l_gain", "L", 948, 458, BAND),
    ("mix", "MIX", 1075, 300, BIG),
    ("upward", "UPWARD", 1210, 56, SMALL), ("downward", "DOWNWARD", 1210, 208, SMALL),
    ("low_xover", "LOW/MID", 1210, 360, SMALL), ("high_xover", "MID/HIGH", 1210, 512, SMALL),
]
QLINKS = [["depth", "time", "in_gain", "out_gain"], ["h_gain", "m_gain", "l_gain", "mix"], ["upward", "downward", "low_xover", "high_xover"]]
BANDS = [("h", 102), ("m", 272), ("l", 442)]   # band rows: the number well's centre y (1.x art)
KEYS = [("fav_add", "SAVE PRESET", 40, 570, 160, 44), ("fav_del", "DELETE PRESET", 210, 570, 160, 44)]


def geometry(size, cx, cy):
    """Caption centre y, value well box, and the control's extent (for the Q-Link frame) for a knob picture of this size."""
    half = size // 2
    if size == BAND:
        cap_y, well = cy + 43, (cx - 60, cy + 56, 120, 28)
    elif size == SMALL:
        cap_y, well = cy + 49, (cx - 64, cy + 61, 128, 28)
    else:
        cap_y, well = cy + 55, (cx - 64, cy + 69, 128, 30)
    ext = (min(cx - half, well[0]) - 2, cy - half - 2, max(cx + half, well[0] + well[2]) + 2, well[1] + well[3] + 3)
    return cap_y, well, ext


def page_art():
    im = Image.new("RGB", (ma.W, ma.H))
    d = ImageDraw.Draw(im)
    ma.vgrad(d, (0, 0, ma.W, ma.H), ma.LIGHT_TOP, ma.LIGHT_BOT)
    ma.vgrad(d, (ma.PANEL_L, 0, ma.PANEL_R, ma.H), ma.TEAL_TOP, ma.TEAL_BOT)
    for x in (ma.PANEL_L, ma.PANEL_R - 1):   # bevel edges
        d.line([(x, 0), (x, ma.H)], fill=(10, 24, 28), width=3)
    for _, y in BANDS:                        # the level number's well (the number strips fill it)
        d.rounded_rectangle([408, y - 22, 492, y + 22], radius=6, fill=WELL_TEAL)
    d.text((40, 50), "OTTmpc", font=ttf(46, "Bold"), fill=INK, anchor="lm")
    d.text((40, 100), "MULTIBAND UP/DOWN COMPRESSOR", font=ttf(17, "Bold"), fill=INK, anchor="lm")
    d.text((500, 566), "-60 dB", font=ttf(11), fill=INK_TEAL, anchor="lm")
    d.text((900, 566), "0 dB", font=ttf(11), fill=INK_TEAL, anchor="rm")
    d.text((700, 566), "BAND LEVEL: INPUT (THIN) / OUTPUT", font=ttf(15), fill=INK_TEAL, anchor="mm")
    for key, cap, cx, cy, size in KNOBS:
        cap_y, (wx, wy, ww, wh), _ = geometry(size, cx, cy)
        dark = size == BAND
        d.text((cx, cap_y), cap, font=ttf(17 if not dark else 16), fill=INK_LIGHT if dark else INK, anchor="mm")
        d.rounded_rectangle([wx, wy, wx + ww - 1, wy + wh - 1], radius=6 if dark else 8, fill=WELL_TEAL if dark else WELL)
    return save(im, "page.png")


class Page:
    def __init__(self):
        self.kids, self.extent = [], {}


def build_page():
    p = Page()
    p.kids.append(entry("bg", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": page_art()}, "0 0 1280 628"))
    strips = {BIG: knob_strip("knob.png", "knob_scale.png", BIG, "knob_big.png"),
              SMALL: knob_strip("knob.png", "knob_scale.png", SMALL, "knob_small.png"),
              BAND: knob_strip("knob_band.png", "knob_band_scale.png", BAND, "knob_band.png")}
    num = meter_strip("num", "meter_num.png")
    inn = meter_strip("in", "meter_in.png")
    for band, y in BANDS:
        pics = [(band + "_out", num, B(408, y - 22, 84, 44)), (band + "_in", inn, B(500, y + 26, 400, 8))]
        if PICTURE_METERS:
            pics.append((band + "_out", meter_strip(band + "_out", "meter_%s_out.png" % band), B(500, y - 20, 400, 40)))
        else:
            off, lit = meter_images(band)
            lv, hd = P(band + "_level"), P(band + "_hold")
            p.kids.append(entry(band + " meter", "Meter", Obj(
                ("version", 3), ("direction", "Right"), ("invert", False), ("inactiveImage", off), ("peakImage", lit), ("rmsImage", lit),
                ("peakHandle", lv), ("rmsHandle", lv), ("peakHoldHandle", hd), ("peakHoldSize", 3), ("peakHoldColour", "fffafafa")),
                B(500, y - 20, ma.BAR_W, ma.BAR_H)))
        for key, pic, b in pics:
            p.kids.append(entry(key, "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": pic, "numFrames": STEPS,
                                               "handleName": "Data"}, b, P(key)))
    for key, cap, cx, cy, size in KNOBS:
        _, (wx, wy, ww, wh), ext = geometry(size, cx, cy)
        p.kids.append(label(B(wx, wy, ww, wh), P(key), h=wh * 0.82, colour="ffe8eef0"))
        p.kids.append(entry(key, knob_def(strips[size], size), {"version": 1, "handleName": "Data"},
                            B(cx - size // 2, cy - size // 2, size, size), P(key), focus="Yes"))
        p.extent[key] = ext
    for key, cap, x, y, w, h in KEYS:
        p.kids.append(entry(key, "Button", {"version": 2, "onImage": save(key_art(w, h, cap, True), key + "_on.png"),
                                            "offImage": save(key_art(w, h, cap, False), key + "_off.png"), "buttonId": 1,
                                            "numButtonsInGroup": 1, "handleName": "Data", "gestureBehaviour": GESTURE}, B(x, y, w, h), P(key)))
    keyboard(p)
    return p


KB_ROWS = [("1234567890", ["kb_bksp"]), ("QWERTYUIOP", []), ("ASDFGHJKL-", []), ("ZXCVBNM", ["kb_space"])]


def kb_key_art(w, h, caption, lit):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w - 1, h - 1), 8, fill=(48, 92, 96) if lit else (26, 52, 58), outline=(143, 176, 170), width=2)
    d.text((w / 2, h / 2 + 1), caption, font=ttf(26, "Bold"), fill=(255, 255, 255) if lit else INK_LIGHT, anchor="mm")
    return im


def slug(k):   # exFAT is case-insensitive: one file name per key, whatever its caption
    return re.sub(r"[^a-z0-9_]", "_", k.lower())


def keyboard(p):
    """The drawn keyboard for naming a user preset: a full-page panel, the name, 42 keys; shown while kb_open = 1."""
    ind = ["IndexedEnabling/1/2/%s" % P("kb_open")]
    panel = Image.new("RGB", (1280, 628))
    d = ImageDraw.Draw(panel)
    ma.vgrad(d, (0, 0, 1280, 628), ma.TEAL_TOP, ma.TEAL_BOT)
    d.rounded_rectangle((42, 14, 1238, 614), 10, outline=(143, 176, 170), width=2)
    d.text((76, 53), "PRESET NAME", font=ttf(22, "Bold"), fill=INK_LIGHT, anchor="lm")
    d.rounded_rectangle((300, 26, 1210, 80), 6, fill=WELL_TEAL, outline=(232, 238, 240), width=2)
    d.text((1196, 53), "MAX %d CHARACTERS" % FAV_NAME_MAX, font=ttf(15), fill=INK_TEAL, anchor="rm")
    p.kids.append(entry("kb panel", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": save(panel, "kb_panel.png")},
                        "0 0 1280 628", ind=ind))
    p.kids.append(label(B(316, 26, 880, 54), P("fav_name"), h=40.0, style="Bold", just="left verticallyCentred", ind=ind))
    kw, kh, gap, x0, y0 = 98, 84, 8, 70, 104

    def key(k, cap, x, y, w):
        p.kids.append(entry(k, "Button", {"version": 2, "onImage": save(kb_key_art(w, kh, cap, True), slug(k) + "_on.png"),
                                          "offImage": save(kb_key_art(w, kh, cap, False), slug(k) + "_off.png"), "buttonId": 1,
                                          "numButtonsInGroup": 1, "handleName": "Data", "gestureBehaviour": GESTURE}, B(x, y, w, kh), P(k), ind=ind))
    for r, (chars, extra) in enumerate(KB_ROWS):
        y = y0 + r * (kh + gap)
        x = x0 + r * 30
        for c in chars:
            key("kb_" + ("dash" if c == "-" else c.lower()), c, x, y, kw)
            x += kw + gap
        for k in extra:
            key(k, {"kb_bksp": "DEL", "kb_space": "SPACE"}[k], x, y, 1210 - x)
    y = y0 + 4 * (kh + gap)
    key("kb_shift", "SHIFT", x0, y, 200)
    key("kb_cancel", "CANCEL", 640 - 130, y, 260)
    key("kb_save", "SAVE", 1210 - 260, y, 260)


def qlink_rects(p):
    rects = []
    for g in QLINKS:
        assert len(g) == 4, g
        ex = [p.extent[k] for k in g]
        rects.append((max(min(e[0] for e in ex), 2), max(min(e[1] for e in ex), 2), min(max(e[2] for e in ex), 1278), min(max(e[3] for e in ex), 626)))
    for i in range(len(rects)):   # the frames must not overlap
        for j in range(i):
            a, b = rects[i], rects[j]
            assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], "Q-Link groups %d and %d overlap: %s %s" % (j, i, a, b)
    return rects


# ---- preview: the page with the widgets at their defaults, as the MPC draws text (offline check) ---------------------------------
def render_preview(p, rects, keyboard_open):
    img = Image.open(os.path.join(OUT, "page.png")).convert("RGBA")
    d = ImageDraw.Draw(img)
    for k in p.kids[1:]:
        shown = "additionalInvalidatingHandles" in k["bounds"]
        if shown != keyboard_open:
            continue
        cd, (x, y, w, h) = k["componentData"], [int(v) for v in k["bounds"]["bounds"].split()]
        typ, data = cd["type"], cd["data"]
        remap = k["handle remapping"]["map"]
        key = LOCK[int(remap[0]["value"].split()[-1])] if remap else None
        spec = PARAMS.get(key, {})
        if typ == "Image" and data["imageType"] == "FilmStrip":
            strip = Image.open(os.path.join(OUT, data["image"])).convert("RGBA")
            fh = strip.height // data["numFrames"]
            f = {"_out": 16, "_in": 14}.get(key[-4:] if key.endswith("_out") else key[-3:], 0)
            img.alpha_composite(strip.crop((0, f * fh, strip.width, f * fh + fh)).resize((w, h)), (x, y))
        elif typ == "Image":
            img.alpha_composite(Image.open(os.path.join(OUT, data["image"])).convert("RGBA"), (x, y))
        elif typ.startswith("knob_"):
            lo, hi = spec.get("min", 0), spec.get("max", 1)
            frac = (spec.get("default", 0) - lo) / (hi - lo) if hi > lo else 0
            strip = Image.open(os.path.join(OUT, DEFS[typ]["componentsData"][1]["componentData"]["data"]["filmStrip"])).convert("RGBA")
            fr = int(round(frac * (FRAMES - 1)))
            img.alpha_composite(strip.crop((0, fr * w, w, fr * w + w)), (x, y))
        elif typ == "Button":
            img.alpha_composite(Image.open(os.path.join(OUT, data["offImage"])).convert("RGBA"), (x, y))
        elif typ == "Label":
            st = data["textStyle"]
            if key == "fav_name":
                txt = "Drum Bus Glue_"
            else:
                v = spec.get("default", 0)
                txt = ("%.0f" % v if (spec.get("max", 0) - spec.get("min", 0)) >= 100 and spec.get("unit") in ("%", "Hz") else "%.1f" % v) + " " + spec.get("unit", "")
            left = "left" in st["justification"]
            d.text((x if left else x + w / 2, y + h / 2), txt, font=ttf(st["font"]["height"] * DEVICE_TEXT, st["font"]["style"]),
                   fill=(232, 238, 240), anchor="lm" if left else "mm")
    if not keyboard_open and not os.environ.get("OTT_PREVIEW_CLEAN"):   # OTT_PREVIEW_CLEAN=1: no Q-Link frames (README pictures)
        for r in rects:
            d.rectangle(r, outline=(232, 60, 60), width=2)
    return img


def main():
    preview = "--preview" in sys.argv
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    p = build_page()
    rects = qlink_rects(p)
    tab = {"version": 1, "tabName": "OTT", "fnKeyIndex": 0, "fnKeySubIndex": 0,
           "componentDefinition": component(p.kids, background=bg("ff10262a")),
           "qlinkBoundsData": [B(r[0], r[1], r[2] - r[0], r[3] - r[1]) for r in rects]}
    q = {"Q-Link %d" % n: -1 for n in range(1, 17)}
    for gi, keys in enumerate(QLINKS):
        for i, key in enumerate(keys):
            q["Q-Link %d" % (13 - 4 * gi + i)] = LOCK.index(key)
    repl = [{"key": P(k), "value": PARAMS[k]["options"]} for k in LOCK if PARAMS.get(k, {}).get("options")]
    defs = [{"key": "knob overlay", "value": overlay_def()}] + [{"key": k, "value": v} for k, v in DEFS.items()]
    tui = {"pageData": {"version": 1,
                        "componentDefinitions": {"version": 2, "importFiles": [], "localComponentDefinitions": defs},
                        "replacementStrings": repl, "info": {"version": 1, "type": "CompleteDescription"},
                        "tabs": [tab]}}
    open(os.path.join(OUT, "TUI.json"), "w").write(dump(tui) + "\n")
    ql = {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
          "Screen Mode Q-Links": {"version": 4, "map": [{"Tab": 1, "SubTab": 1, "Bank Direction": "RowTopDown", "Q-Links": q}]},
          "Program Mode Q-Links": {"Q-Link %d" % n: -1 for n in range(1, 17)}}
    open(os.path.join(OUT, "Q-Links.json"), "w").write(dump(ql) + "\n")
    names = [f.lower() for f in os.listdir(OUT)]
    assert len(names) == len(set(names)), "file names clash on a case-insensitive drive"
    if preview:
        os.makedirs(PREVIEW, exist_ok=True)
        render_preview(p, rects, False).convert("RGB").save(os.path.join(PREVIEW, "ott.png"))
        render_preview(p, rects, True).convert("RGB").save(os.path.join(PREVIEW, "ott_keyboard.png"))
    print("wrote %s - 1 tab, %d files, %d KB" % (OUT, len(names), sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT)) // 1024))


if __name__ == "__main__":
    main()
