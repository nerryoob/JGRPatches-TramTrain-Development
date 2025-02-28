/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file gfx.cpp Handling of drawing text and other gfx related stuff. */

#include "stdafx.h"
#include "gfx_layout.h"
#include "progress.h"
#include "zoom_func.h"
#include "blitter/factory.hpp"
#include "video/video_driver.hpp"
#include "strings_func.h"
#include "settings_type.h"
#include "network/network.h"
#include "network/network_func.h"
#include "window_gui.h"
#include "window_func.h"
#include "newgrf_debug.h"
#include "thread.h"
#include "widget_type.h"
#include "window_gui.h"
#include "framerate_type.h"
#include "transparency.h"
#include "core/backup_type.hpp"
#include "viewport_func.h"

#include "table/palettes.h"
#include "table/string_colours.h"
#include "table/sprites.h"
#include "table/control_codes.h"

#include <atomic>

#include "safeguards.h"

byte _dirkeys;        ///< 1 = left, 2 = up, 4 = right, 8 = down
bool _fullscreen;
byte _support8bpp;
CursorVars _cursor;
bool _ctrl_pressed;   ///< Is Ctrl pressed?
bool _shift_pressed;  ///< Is Shift pressed?
bool _invert_ctrl;
bool _invert_shift;
uint16 _game_speed = 100; ///< Current game-speed; 100 is 1x, 0 is infinite.
uint8 _milliseconds_per_tick = 27; ///< Milliseconds per tick
float _ticks_per_second;           ///< Ticks per second
bool _left_button_down;     ///< Is left mouse button pressed?
bool _left_button_clicked;  ///< Is left mouse button clicked?
bool _right_button_down;    ///< Is right mouse button pressed?
bool _right_button_clicked; ///< Is right mouse button clicked?
DrawPixelInfo _screen;
bool _screen_disable_anim = false;   ///< Disable palette animation (important for 32bpp-anim blitter during giant screenshot)
bool _check_special_modes;
std::atomic<bool> _exit_game;
GameMode _game_mode;
SwitchMode _switch_mode;  ///< The next mainloop command.
PauseMode _pause_mode;
uint32 _pause_countdown;
Palette _cur_palette;
std::mutex _cur_palette_mutex;
std::string _switch_baseset;
static bool _adjust_gui_zoom_startup_done = false;

static byte _stringwidth_table[FS_END][224]; ///< Cache containing width of often used characters. @see GetCharacterWidth()
DrawPixelInfo *_cur_dpi;
byte _colour_gradient[COLOUR_END][8];

byte _colour_value[COLOUR_END] = {
	133, // COLOUR_DARK_BLUE
	 99, // COLOUR_PALE_GREEN,
	 48, // COLOUR_PINK,
	 68, // COLOUR_YELLOW,
	184, // COLOUR_RED,
	152, // COLOUR_LIGHT_BLUE,
	209, // COLOUR_GREEN,
	 95, // COLOUR_DARK_GREEN,
	150, // COLOUR_BLUE,
	 79, // COLOUR_CREAM,
	134, // COLOUR_MAUVE,
	174, // COLOUR_PURPLE,
	195, // COLOUR_ORANGE,
	116, // COLOUR_BROWN,
	  6, // COLOUR_GREY,
	 15, // COLOUR_WHITE,
};

struct GfxBlitterCtx {
	const DrawPixelInfo *dpi;
	const byte *colour_remap_ptr = nullptr;
	byte string_colourremap[3]; ///< Recoloursprite for stringdrawing. The grf loader ensures that #SpriteType::Font sprites only use colours 0 to 2.
	int sprite_brightness_adjust = 0;

	GfxBlitterCtx(const DrawPixelInfo *dpi) : dpi(dpi) {}

	void SetColourRemap(TextColour colour);
};

static void GfxMainBlitterViewport(const GfxBlitterCtx &ctx, const Sprite *sprite, int x, int y, BlitterMode mode, const SubSprite *sub = nullptr, SpriteID sprite_id = SPR_CURSOR_MOUSE);
static void GfxMainBlitter(const GfxBlitterCtx &ctx, const Sprite *sprite, int x, int y, BlitterMode mode, const SubSprite *sub = nullptr, SpriteID sprite_id = SPR_CURSOR_MOUSE, ZoomLevel zoom = ZOOM_LVL_NORMAL);

static ReusableBuffer<uint8> _cursor_backup;

ZoomLevel _gui_zoom  = ZOOM_LVL_OUT_4X;     ///< GUI Zoom level
ZoomLevel _font_zoom = _gui_zoom;           ///< Sprite font Zoom level (not clamped)
int _gui_scale       = MIN_INTERFACE_SCALE; ///< GUI scale, 100 is 100%.
int _gui_scale_cfg;                         ///< GUI scale in config.

/**
 * The rect for repaint.
 *
 * This rectangle defines the area which should be repaint by the video driver.
 *
 * @ingroup dirty
 */

extern std::atomic<uint> _dirty_block_colour;
static bool _whole_screen_dirty = false;
bool _gfx_draw_active = false;

static std::vector<Rect> _dirty_blocks;
static std::vector<Rect> _pending_dirty_blocks;

enum GfxDebugFlags {
	GDF_SHOW_WINDOW_DIRTY,
	GDF_SHOW_WIDGET_DIRTY,
	GDF_SHOW_RECT_DIRTY,
};
uint32 _gfx_debug_flags;

/**
 * Applies a certain FillRectMode-operation to a rectangle [left, right] x [top, bottom] on the screen.
 *
 * @pre dpi->zoom == ZOOM_LVL_NORMAL, right >= left, bottom >= top
 * @param dpi Draw pixel info
 * @param left Minimum X (inclusive)
 * @param top Minimum Y (inclusive)
 * @param right Maximum X (inclusive)
 * @param bottom Maximum Y (inclusive)
 * @param colour A 8 bit palette index (FILLRECT_OPAQUE and FILLRECT_CHECKER) or a recolour spritenumber (FILLRECT_RECOLOUR)
 * @param mode
 *         FILLRECT_OPAQUE:   Fill the rectangle with the specified colour
 *         FILLRECT_CHECKER:  Like FILLRECT_OPAQUE, but only draw every second pixel (used to grey out things)
 *         FILLRECT_RECOLOUR:  Apply a recolour sprite to every pixel in the rectangle currently on screen
 */
void GfxFillRect(const DrawPixelInfo *dpi, int left, int top, int right, int bottom, int colour, FillRectMode mode)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	void *dst;
	const int otop = top;
	const int oleft = left;

	if (dpi->zoom != ZOOM_LVL_NORMAL) return;
	if (left > right || top > bottom) return;
	if (right < dpi->left || left >= dpi->left + dpi->width) return;
	if (bottom < dpi->top || top >= dpi->top + dpi->height) return;

	if ( (left -= dpi->left) < 0) left = 0;
	right = right - dpi->left + 1;
	if (right > dpi->width) right = dpi->width;
	right -= left;
	assert(right > 0);

	if ( (top -= dpi->top) < 0) top = 0;
	bottom = bottom - dpi->top + 1;
	if (bottom > dpi->height) bottom = dpi->height;
	bottom -= top;
	assert(bottom > 0);

	dst = blitter->MoveTo(dpi->dst_ptr, left, top);

	switch (mode) {
		default: // FILLRECT_OPAQUE
			blitter->DrawRect(dst, right, bottom, (uint8)colour);
			break;

		case FILLRECT_RECOLOUR:
			blitter->DrawColourMappingRect(dst, right, bottom, GB(colour, 0, PALETTE_WIDTH));
			break;

		case FILLRECT_CHECKER: {
			byte bo = (oleft - left + dpi->left + otop - top + dpi->top) & 1;
			do {
				for (int i = (bo ^= 1); i < right; i += 2) blitter->SetPixel(dst, i, 0, (uint8)colour);
				dst = blitter->MoveTo(dst, 0, 1);
			} while (--bottom > 0);
			break;
		}
	}
}

typedef std::pair<Point, Point> LineSegment;

/**
 * Make line segments from a polygon defined by points, translated by an offset.
 * Entirely horizontal lines (start and end at same Y coordinate) are skipped, as they are irrelevant to scanline conversion algorithms.
 * Generated line segments always have the lowest Y coordinate point first, i.e. original direction is lost.
 * @param shape The polygon to convert.
 * @param offset Offset vector subtracted from all coordinates in the shape.
 * @return Vector of undirected line segments.
 */
static std::vector<LineSegment> MakePolygonSegments(const std::vector<Point> &shape, Point offset)
{
	std::vector<LineSegment> segments;
	if (shape.size() < 3) return segments; // fewer than 3 will always result in an empty polygon
	segments.reserve(shape.size());

	/* Connect first and last point by having initial previous point be the last */
	Point prev = shape.back();
	prev.x -= offset.x;
	prev.y -= offset.y;
	for (Point pt : shape) {
		pt.x -= offset.x;
		pt.y -= offset.y;
		/* Create segments for all non-horizontal lines in the polygon.
		 * The segments always have lowest Y coordinate first. */
		if (prev.y > pt.y) {
			segments.emplace_back(pt, prev);
		} else if (prev.y < pt.y) {
			segments.emplace_back(prev, pt);
		}
		prev = pt;
	}

	return segments;
}

/**
 * Fill a polygon with colour.
 * The odd-even winding rule is used, i.e. self-intersecting polygons will have holes in them.
 * Left and top edges are inclusive, right and bottom edges are exclusive.
 * @note For rectangles the GfxFillRect function will be faster.
 * @pre dpi->zoom == ZOOM_LVL_NORMAL
 * @param shape List of points on the polygon.
 * @param colour An 8 bit palette index (FILLRECT_OPAQUE and FILLRECT_CHECKER) or a recolour spritenumber (FILLRECT_RECOLOUR).
 * @param mode
 *         FILLRECT_OPAQUE:   Fill the polygon with the specified colour.
 *         FILLRECT_CHECKER:  Fill every other pixel with the specified colour, in a checkerboard pattern.
 *         FILLRECT_RECOLOUR: Apply a recolour sprite to every pixel in the polygon.
 *         FILLRECT_FUNCTOR:  Apply a functor to a line of pixels.
 */
void GfxFillPolygon(const std::vector<Point> &shape, int colour, FillRectMode mode, GfxFillRectModeFunctor *fill_functor)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	const DrawPixelInfo *dpi = _cur_dpi;
	if (dpi->zoom != ZOOM_LVL_NORMAL) return;

	std::vector<LineSegment> segments = MakePolygonSegments(shape, Point{ dpi->left, dpi->top });

	/* Remove segments appearing entirely above or below the clipping area. */
	segments.erase(std::remove_if(segments.begin(), segments.end(), [dpi](const LineSegment &s) { return s.second.y <= 0 || s.first.y >= dpi->height; }), segments.end());

	/* Check that this wasn't an empty shape (all points on a horizontal line or outside clipping.) */
	if (segments.empty()) return;

	/* Sort the segments by first point Y coordinate. */
	std::sort(segments.begin(), segments.end(), [](const LineSegment &a, const LineSegment &b) { return a.first.y < b.first.y; });

	/* Segments intersecting current scanline. */
	std::vector<LineSegment> active;
	/* Intersection points with a scanline.
	 * Kept outside loop to avoid repeated re-allocations. */
	std::vector<int> intersections;
	/* Normal, reasonable polygons don't have many intersections per scanline. */
	active.reserve(4);
	intersections.reserve(4);

	/* Scan through the segments and paint each scanline. */
	int y = segments.front().first.y;
	std::vector<LineSegment>::iterator nextseg = segments.begin();
	while (!active.empty() || nextseg != segments.end()) {
		/* Clean up segments that have ended. */
		active.erase(std::remove_if(active.begin(), active.end(), [y](const LineSegment &s) { return s.second.y == y; }), active.end());

		/* Activate all segments starting on this scanline. */
		while (nextseg != segments.end() && nextseg->first.y == y) {
			active.push_back(*nextseg);
			++nextseg;
		}

		/* Check clipping. */
		if (y < 0) {
			++y;
			continue;
		}
		if (y >= dpi->height) return;

		/*  Intersect scanline with all active segments. */
		intersections.clear();
		for (const LineSegment &s : active) {
			const int sdx = s.second.x - s.first.x;
			const int sdy = s.second.y - s.first.y;
			const int ldy = y - s.first.y;
			const int x = s.first.x + sdx * ldy / sdy;
			intersections.push_back(x);
		}

		/* Fill between pairs of intersections. */
		std::sort(intersections.begin(), intersections.end());
		for (size_t i = 1; i < intersections.size(); i += 2) {
			/* Check clipping. */
			const int x1 = std::max(0, intersections[i - 1]);
			const int x2 = std::min(intersections[i], dpi->width);
			if (x2 < 0) continue;
			if (x1 >= dpi->width) continue;

			/* Fill line y from x1 to x2. */
			void *dst = blitter->MoveTo(dpi->dst_ptr, x1, y);
			switch (mode) {
				default: // FILLRECT_OPAQUE
					blitter->DrawRect(dst, x2 - x1, 1, (uint8)colour);
					break;
				case FILLRECT_RECOLOUR:
					blitter->DrawColourMappingRect(dst, x2 - x1, 1, GB(colour, 0, PALETTE_WIDTH));
					break;
				case FILLRECT_CHECKER:
					/* Fill every other pixel, offset such that the sum of filled pixels' X and Y coordinates is odd.
					 * This creates a checkerboard effect. */
					for (int x = (x1 + y) & 1; x < x2 - x1; x += 2) {
						blitter->SetPixel(dst, x, 0, (uint8)colour);
					}
					break;
				case FILLRECT_FUNCTOR:
					/* Call the provided fill functor. */
					fill_functor(dst, x2 - x1);
					break;
			}
		}

		/* Next line */
		++y;
	}
}

/**
 * Check line clipping by using a linear equation and draw the visible part of
 * the line given by x/y and x2/y2.
 * @param video Destination pointer to draw into.
 * @param x X coordinate of first point.
 * @param y Y coordinate of first point.
 * @param x2 X coordinate of second point.
 * @param y2 Y coordinate of second point.
 * @param screen_width With of the screen to check clipping against.
 * @param screen_height Height of the screen to check clipping against.
 * @param colour Colour of the line.
 * @param width Width of the line.
 * @param dash Length of dashes for dashed lines. 0 means solid line.
 */
static inline void GfxDoDrawLine(void *video, int x, int y, int x2, int y2, int screen_width, int screen_height, uint8 colour, int width, int dash = 0)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();

	assert(width > 0);

	if (y2 == y || x2 == x) {
		/* Special case: horizontal/vertical line. All checks already done in GfxPreprocessLine. */
		blitter->DrawLine(video, x, y, x2, y2, screen_width, screen_height, colour, width, dash);
		return;
	}

	int grade_y = y2 - y;
	int grade_x = x2 - x;

	/* Clipping rectangle. Slightly extended so we can ignore the width of the line. */
	int extra = (int)CeilDiv(3 * width, 4); // not less then "width * sqrt(2) / 2"
	Rect clip = { -extra, -extra, screen_width - 1 + extra, screen_height - 1 + extra };

	/* prevent integer overflows. */
	int margin = 1;
	while (INT_MAX / abs(grade_y) < std::max(abs(clip.left - x), abs(clip.right - x))) {
		grade_y /= 2;
		grade_x /= 2;
		margin  *= 2; // account for rounding errors
	}

	/* Imagine that the line is infinitely long and it intersects with
	 * infinitely long left and right edges of the clipping rectangle.
	 * If both intersection points are outside the clipping rectangle
	 * and both on the same side of it, we don't need to draw anything. */
	int left_isec_y = y + (clip.left - x) * grade_y / grade_x;
	int right_isec_y = y + (clip.right - x) * grade_y / grade_x;
	if ((left_isec_y > clip.bottom + margin && right_isec_y > clip.bottom + margin) ||
			(left_isec_y < clip.top - margin && right_isec_y < clip.top - margin)) {
		return;
	}

	/* It is possible to use the line equation to further reduce the amount of
	 * work the blitter has to do by shortening the effective line segment.
	 * However, in order to get that right and prevent the flickering effects
	 * of rounding errors so much additional code has to be run here that in
	 * the general case the effect is not noticeable. */

	blitter->DrawLine(video, x, y, x2, y2, screen_width, screen_height, colour, width, dash);
}

/**
 * Align parameters of a line to the given DPI and check simple clipping.
 * @param dpi Screen parameters to align with.
 * @param x X coordinate of first point.
 * @param y Y coordinate of first point.
 * @param x2 X coordinate of second point.
 * @param y2 Y coordinate of second point.
 * @param width Width of the line.
 * @return True if the line is likely to be visible, false if it's certainly
 *         invisible.
 */
static inline bool GfxPreprocessLine(const DrawPixelInfo *dpi, int &x, int &y, int &x2, int &y2, int width)
{
	x -= dpi->left;
	x2 -= dpi->left;
	y -= dpi->top;
	y2 -= dpi->top;

	/* Check simple clipping */
	if (x + width / 2 < 0           && x2 + width / 2 < 0          ) return false;
	if (y + width / 2 < 0           && y2 + width / 2 < 0          ) return false;
	if (x - width / 2 > dpi->width  && x2 - width / 2 > dpi->width ) return false;
	if (y - width / 2 > dpi->height && y2 - width / 2 > dpi->height) return false;
	return true;
}

void GfxDrawLine(const DrawPixelInfo *dpi, int x, int y, int x2, int y2, int colour, int width, int dash)
{
	if (GfxPreprocessLine(dpi, x, y, x2, y2, width)) {
		GfxDoDrawLine(dpi->dst_ptr, x, y, x2, y2, dpi->width, dpi->height, colour, width, dash);
	}
}

void GfxDrawLineUnscaled(const DrawPixelInfo *dpi, int x, int y, int x2, int y2, int colour)
{
	if (GfxPreprocessLine(dpi, x, y, x2, y2, 1)) {
		GfxDoDrawLine(dpi->dst_ptr,
				UnScaleByZoom(x, dpi->zoom), UnScaleByZoom(y, dpi->zoom),
				UnScaleByZoom(x2, dpi->zoom), UnScaleByZoom(y2, dpi->zoom),
				UnScaleByZoom(dpi->width, dpi->zoom), UnScaleByZoom(dpi->height, dpi->zoom), colour, 1);
	}
}

/**
 * Draws the projection of a parallelepiped.
 * This can be used to draw boxes in world coordinates.
 *
 * @param dpi Draw pixel info.
 * @param x   Screen X-coordinate of top front corner.
 * @param y   Screen Y-coordinate of top front corner.
 * @param dx1 Screen X-length of first edge.
 * @param dy1 Screen Y-length of first edge.
 * @param dx2 Screen X-length of second edge.
 * @param dy2 Screen Y-length of second edge.
 * @param dx3 Screen X-length of third edge.
 * @param dy3 Screen Y-length of third edge.
 */
void DrawBox(const DrawPixelInfo *dpi, int x, int y, int dx1, int dy1, int dx2, int dy2, int dx3, int dy3)
{
	/*           ....
	 *         ..    ....
	 *       ..          ....
	 *     ..                ^
	 *   <--__(dx1,dy1)    /(dx2,dy2)
	 *   :    --__       /   :
	 *   :        --__ /     :
	 *   :            *(x,y) :
	 *   :            |      :
	 *   :            |     ..
	 *    ....        |(dx3,dy3)
	 *        ....    | ..
	 *            ....V.
	 */

	static const byte colour = PC_WHITE;

	GfxDrawLineUnscaled(dpi, x, y, x + dx1, y + dy1, colour);
	GfxDrawLineUnscaled(dpi, x, y, x + dx2, y + dy2, colour);
	GfxDrawLineUnscaled(dpi, x, y, x + dx3, y + dy3, colour);

	GfxDrawLineUnscaled(dpi, x + dx1, y + dy1, x + dx1 + dx2, y + dy1 + dy2, colour);
	GfxDrawLineUnscaled(dpi, x + dx1, y + dy1, x + dx1 + dx3, y + dy1 + dy3, colour);
	GfxDrawLineUnscaled(dpi, x + dx2, y + dy2, x + dx2 + dx1, y + dy2 + dy1, colour);
	GfxDrawLineUnscaled(dpi, x + dx2, y + dy2, x + dx2 + dx3, y + dy2 + dy3, colour);
	GfxDrawLineUnscaled(dpi, x + dx3, y + dy3, x + dx3 + dx1, y + dy3 + dy1, colour);
	GfxDrawLineUnscaled(dpi, x + dx3, y + dy3, x + dx3 + dx2, y + dy3 + dy2, colour);
}

/**
 * Set the colour remap to be for the given colour.
 * @param colour the new colour of the remap.
 */
void GfxBlitterCtx::SetColourRemap(TextColour colour)
{
	if (colour == TC_INVALID) return;

	/* Black strings have no shading ever; the shading is black, so it
	 * would be invisible at best, but it actually makes it illegible. */
	bool no_shade   = (colour & TC_NO_SHADE) != 0 || (colour & ~TC_FORCED) == TC_BLACK;
	bool raw_colour = (colour & TC_IS_PALETTE_COLOUR) != 0;
	colour &= ~(TC_NO_SHADE | TC_IS_PALETTE_COLOUR | TC_FORCED);

	this->string_colourremap[1] = raw_colour ? (byte)colour : _string_colourmap[colour];
	this->string_colourremap[2] = no_shade ? 0 : 1;
	this->colour_remap_ptr = this->string_colourremap;
}

/**
 * Drawing routine for drawing a laid out line of text.
 * @param line      String to draw.
 * @param y         The top most position to draw on.
 * @param left      The left most position to draw on.
 * @param right     The right most position to draw on.
 * @param align     The alignment of the string when drawing left-to-right. In the
 *                  case a right-to-left language is chosen this is inverted so it
 *                  will be drawn in the right direction.
 * @param underline Whether to underline what has been drawn or not.
 * @param truncation Whether to perform string truncation or not.
 *
 * @return In case of left or center alignment the right most pixel we have drawn to.
 *         In case of right alignment the left most pixel we have drawn to.
 */
static int DrawLayoutLine(const ParagraphLayouter::Line &line, int y, int left, int right, StringAlignment align, bool underline, bool truncation)
{
	if (line.CountRuns() == 0) return 0;

	int w = line.GetWidth();
	int h = line.GetLeading();

	/*
	 * The following is needed for truncation.
	 * Depending on the text direction, we either remove bits at the rear
	 * or the front. For this we shift the entire area to draw so it fits
	 * within the left/right bounds and the side we do not truncate it on.
	 * Then we determine the truncation location, i.e. glyphs that fall
	 * outside of the range min_x - max_x will not be drawn; they are thus
	 * the truncated glyphs.
	 *
	 * At a later step we insert the dots.
	 */

	int max_w = right - left + 1; // The maximum width.

	int offset_x = 0;  // The offset we need for positioning the glyphs
	int min_x = left;  // The minimum x position to draw normal glyphs on.
	int max_x = right; // The maximum x position to draw normal glyphs on.

	truncation &= max_w < w;         // Whether we need to do truncation.
	int dot_width = 0;               // Cache for the width of the dot.
	const Sprite *dot_sprite = nullptr; // Cache for the sprite of the dot.

	if (truncation) {
		/*
		 * Assumption may be made that all fonts of a run are of the same size.
		 * In any case, we'll use these dots for the abbreviation, so even if
		 * another size would be chosen it won't have truncated too little for
		 * the truncation dots.
		 */
		FontCache *fc = ((const Font*)line.GetVisualRun(0).GetFont())->fc;
		GlyphID dot_glyph = fc->MapCharToGlyph('.');
		dot_width = fc->GetGlyphWidth(dot_glyph);
		dot_sprite = fc->GetGlyph(dot_glyph);

		if (_current_text_dir == TD_RTL) {
			min_x += 3 * dot_width;
			offset_x = w - 3 * dot_width - max_w;
		} else {
			max_x -= 3 * dot_width;
		}

		w = max_w;
	}

	/* In case we have a RTL language we swap the alignment. */
	if (!(align & SA_FORCE) && _current_text_dir == TD_RTL && (align & SA_HOR_MASK) != SA_HOR_CENTER) align ^= SA_RIGHT;

	/* right is the right most position to draw on. In this case we want to do
	 * calculations with the width of the string. In comparison right can be
	 * seen as lastof(todraw) and width as lengthof(todraw). They differ by 1.
	 * So most +1/-1 additions are to move from lengthof to 'indices'.
	 */
	switch (align & SA_HOR_MASK) {
		case SA_LEFT:
			/* right + 1 = left + w */
			right = left + w - 1;
			break;

		case SA_HOR_CENTER:
			left  = RoundDivSU(right + 1 + left - w, 2);
			/* right + 1 = left + w */
			right = left + w - 1;
			break;

		case SA_RIGHT:
			left = right + 1 - w;
			break;

		default:
			NOT_REACHED();
	}

	GfxBlitterCtx ctx(_cur_dpi);

	const uint shadow_offset = ScaleGUITrad(1);

	TextColour colour = TC_BLACK;
	bool draw_shadow = false;
	for (int run_index = 0; run_index < line.CountRuns(); run_index++) {
		const ParagraphLayouter::VisualRun &run = line.GetVisualRun(run_index);
		const Font *f = (const Font*)run.GetFont();

		FontCache *fc = f->fc;
		colour = f->colour;
		ctx.SetColourRemap(colour);

		DrawPixelInfo *dpi = _cur_dpi;
		int dpi_left  = dpi->left;
		int dpi_right = dpi->left + dpi->width - 1;

		draw_shadow = fc->GetDrawGlyphShadow() && (colour & TC_NO_SHADE) == 0 && (colour & ~TC_FORCED) != TC_BLACK;

		for (int i = 0; i < run.GetGlyphCount(); i++) {
			GlyphID glyph = run.GetGlyphs()[i];

			/* Not a valid glyph (empty) */
			if (glyph == 0xFFFF) continue;

			int begin_x = (int)run.GetPositions()[i * 2]     + left - offset_x;
			int end_x   = (int)run.GetPositions()[i * 2 + 2] + left - offset_x  - 1;
			int top     = (int)run.GetPositions()[i * 2 + 1] + y;

			/* Truncated away. */
			if (truncation && (begin_x < min_x || end_x > max_x)) continue;

			const Sprite *sprite = fc->GetGlyph(glyph);
			/* Check clipping (the "+ 1" is for the shadow). */
			if (begin_x + sprite->x_offs > dpi_right || begin_x + sprite->x_offs + sprite->width /* - 1 + 1 */ < dpi_left) continue;

			if (draw_shadow && (glyph & SPRITE_GLYPH) == 0) {
				ctx.SetColourRemap(TC_BLACK);
				GfxMainBlitter(ctx, sprite, begin_x + shadow_offset, top + shadow_offset, BM_COLOUR_REMAP);
				ctx.SetColourRemap(colour);
			}
			GfxMainBlitter(ctx, sprite, begin_x, top, BM_COLOUR_REMAP);
		}
	}

	if (truncation) {
		int x = (_current_text_dir == TD_RTL) ? left : (right - 3 * dot_width);
		for (int i = 0; i < 3; i++, x += dot_width) {
			if (draw_shadow) {
				ctx.SetColourRemap(TC_BLACK);
				GfxMainBlitter(ctx, dot_sprite, x + shadow_offset, y + shadow_offset, BM_COLOUR_REMAP);
				ctx.SetColourRemap(colour);
			}
			GfxMainBlitter(ctx, dot_sprite, x, y, BM_COLOUR_REMAP);
		}
	}

	if (underline) {
		GfxFillRect(left, y + h, right, y + h, ctx.string_colourremap[1]);
	}

	return (align & SA_HOR_MASK) == SA_RIGHT ? left : right;
}

/**
 * Draw string, possibly truncated to make it fit in its allocated space
 *
 * @param left   The left most position to draw on.
 * @param right  The right most position to draw on.
 * @param top    The top most position to draw on.
 * @param str    String to draw.
 * @param colour Colour used for drawing the string, for details see _string_colourmap in
 *               table/palettes.h or docs/ottd-colourtext-palette.png or the enum TextColour in gfx_type.h
 * @param align  The alignment of the string when drawing left-to-right. In the
 *               case a right-to-left language is chosen this is inverted so it
 *               will be drawn in the right direction.
 * @param underline Whether to underline what has been drawn or not.
 * @param fontsize The size of the initial characters.
 * @return In case of left or center alignment the right most pixel we have drawn to.
 *         In case of right alignment the left most pixel we have drawn to.
 */
int DrawString(int left, int right, int top, const char *str, TextColour colour, StringAlignment align, bool underline, FontSize fontsize)
{
	/* The string may contain control chars to change the font, just use the biggest font for clipping. */
	int max_height = std::max({FONT_HEIGHT_SMALL, FONT_HEIGHT_NORMAL, FONT_HEIGHT_LARGE, FONT_HEIGHT_MONO});

	/* Funny glyphs may extent outside the usual bounds, so relax the clipping somewhat. */
	int extra = max_height / 2;

	if (_cur_dpi->top + _cur_dpi->height + extra < top || _cur_dpi->top > top + max_height + extra ||
			_cur_dpi->left + _cur_dpi->width + extra < left || _cur_dpi->left > right + extra) {
		return 0;
	}

	Layouter layout(str, INT32_MAX, colour, fontsize);
	if (layout.size() == 0) return 0;

	return DrawLayoutLine(*layout.front(), top, left, right, align, underline, true);
}

/**
 * Draw string, possibly truncated to make it fit in its allocated space
 *
 * @param left   The left most position to draw on.
 * @param right  The right most position to draw on.
 * @param top    The top most position to draw on.
 * @param str    String to draw.
 * @param colour Colour used for drawing the string, for details see _string_colourmap in
 *               table/palettes.h or docs/ottd-colourtext-palette.png or the enum TextColour in gfx_type.h
 * @param align  The alignment of the string when drawing left-to-right. In the
 *               case a right-to-left language is chosen this is inverted so it
 *               will be drawn in the right direction.
 * @param underline Whether to underline what has been drawn or not.
 * @param fontsize The size of the initial characters.
 * @return In case of left or center alignment the right most pixel we have drawn to.
 *         In case of right alignment the left most pixel we have drawn to.
 */
int DrawString(int left, int right, int top, const std::string &str, TextColour colour, StringAlignment align, bool underline, FontSize fontsize)
{
	return DrawString(left, right, top, str.c_str(), colour, align, underline, fontsize);
}

/**
 * Draw string, possibly truncated to make it fit in its allocated space
 *
 * @param left   The left most position to draw on.
 * @param right  The right most position to draw on.
 * @param top    The top most position to draw on.
 * @param str    String to draw.
 * @param colour Colour used for drawing the string, for details see _string_colourmap in
 *               table/palettes.h or docs/ottd-colourtext-palette.png or the enum TextColour in gfx_type.h
 * @param align  The alignment of the string when drawing left-to-right. In the
 *               case a right-to-left language is chosen this is inverted so it
 *               will be drawn in the right direction.
 * @param underline Whether to underline what has been drawn or not.
 * @param fontsize The size of the initial characters.
 * @return In case of left or center alignment the right most pixel we have drawn to.
 *         In case of right alignment the left most pixel we have drawn to.
 */
int DrawString(int left, int right, int top, StringID str, TextColour colour, StringAlignment align, bool underline, FontSize fontsize)
{
	char buffer[DRAW_STRING_BUFFER];
	GetString(buffer, str, lastof(buffer));
	return DrawString(left, right, top, buffer, colour, align, underline, fontsize);
}

/**
 * Calculates height of string (in pixels). The string is changed to a multiline string if needed.
 * @param str string to check
 * @param maxw maximum string width
 * @return height of pixels of string when it is drawn
 */
int GetStringHeight(const char *str, int maxw, FontSize fontsize)
{
	Layouter layout(str, maxw, TC_FROMSTRING, fontsize);
	return layout.GetBounds().height;
}

/**
 * Calculates height of string (in pixels). The string is changed to a multiline string if needed.
 * @param str string to check
 * @param maxw maximum string width
 * @return height of pixels of string when it is drawn
 */
int GetStringHeight(StringID str, int maxw)
{
	char buffer[DRAW_STRING_BUFFER];
	GetString(buffer, str, lastof(buffer));
	return GetStringHeight(buffer, maxw);
}

/**
 * Calculates number of lines of string. The string is changed to a multiline string if needed.
 * @param str string to check
 * @param maxw maximum string width
 * @return number of lines of string when it is drawn
 */
int GetStringLineCount(StringID str, int maxw)
{
	char buffer[DRAW_STRING_BUFFER];
	GetString(buffer, str, lastof(buffer));

	Layouter layout(buffer, maxw);
	return (uint)layout.size();
}

/**
 * Calculate string bounding box for multi-line strings.
 * @param str        String to check.
 * @param suggestion Suggested bounding box.
 * @return Bounding box for the multi-line string, may be bigger than \a suggestion.
 */
Dimension GetStringMultiLineBoundingBox(StringID str, const Dimension &suggestion)
{
	Dimension box = {suggestion.width, (uint)GetStringHeight(str, suggestion.width)};
	return box;
}

/**
 * Calculate string bounding box for multi-line strings.
 * @param str        String to check.
 * @param suggestion Suggested bounding box.
 * @return Bounding box for the multi-line string, may be bigger than \a suggestion.
 */
Dimension GetStringMultiLineBoundingBox(const char *str, const Dimension &suggestion)
{
	Dimension box = {suggestion.width, (uint)GetStringHeight(str, suggestion.width)};
	return box;
}

/**
 * Draw string, possibly over multiple lines.
 *
 * @param left   The left most position to draw on.
 * @param right  The right most position to draw on.
 * @param top    The top most position to draw on.
 * @param bottom The bottom most position to draw on.
 * @param str    String to draw.
 * @param colour Colour used for drawing the string, for details see _string_colourmap in
 *               table/palettes.h or docs/ottd-colourtext-palette.png or the enum TextColour in gfx_type.h
 * @param align  The horizontal and vertical alignment of the string.
 * @param underline Whether to underline all strings
 * @param fontsize The size of the initial characters.
 *
 * @return If \a align is #SA_BOTTOM, the top to where we have written, else the bottom to where we have written.
 */
int DrawStringMultiLine(int left, int right, int top, int bottom, const char *str, TextColour colour, StringAlignment align, bool underline, FontSize fontsize)
{
	int maxw = right - left + 1;
	int maxh = bottom - top + 1;

	/* It makes no sense to even try if it can't be drawn anyway, or
	 * do we really want to support fonts of 0 or less pixels high? */
	if (maxh <= 0) return top;

	Layouter layout(str, maxw, colour, fontsize);
	int total_height = layout.GetBounds().height;
	int y;
	switch (align & SA_VERT_MASK) {
		case SA_TOP:
			y = top;
			break;

		case SA_VERT_CENTER:
			y = RoundDivSU(bottom + top - total_height, 2);
			break;

		case SA_BOTTOM:
			y = bottom - total_height;
			break;

		default: NOT_REACHED();
	}

	int last_line = top;
	int first_line = bottom;

	for (const auto &line : layout) {

		int line_height = line->GetLeading();
		if (y >= top && y + line_height - 1 <= bottom) {
			last_line = y + line_height;
			if (first_line > y) first_line = y;

			DrawLayoutLine(*line, y, left, right, align, underline, false);
		}
		y += line_height;
	}

	return ((align & SA_VERT_MASK) == SA_BOTTOM) ? first_line : last_line;
}


/**
 * Draw string, possibly over multiple lines.
 *
 * @param left   The left most position to draw on.
 * @param right  The right most position to draw on.
 * @param top    The top most position to draw on.
 * @param bottom The bottom most position to draw on.
 * @param str    String to draw.
 * @param colour Colour used for drawing the string, for details see _string_colourmap in
 *               table/palettes.h or docs/ottd-colourtext-palette.png or the enum TextColour in gfx_type.h
 * @param align  The horizontal and vertical alignment of the string.
 * @param underline Whether to underline all strings
 * @param fontsize The size of the initial characters.
 *
 * @return If \a align is #SA_BOTTOM, the top to where we have written, else the bottom to where we have written.
 */
int DrawStringMultiLine(int left, int right, int top, int bottom, const std::string &str, TextColour colour, StringAlignment align, bool underline, FontSize fontsize)
{
	return DrawStringMultiLine(left, right, top, bottom, str.c_str(), colour, align, underline, fontsize);
}

/**
 * Draw string, possibly over multiple lines.
 *
 * @param left   The left most position to draw on.
 * @param right  The right most position to draw on.
 * @param top    The top most position to draw on.
 * @param bottom The bottom most position to draw on.
 * @param str    String to draw.
 * @param colour Colour used for drawing the string, for details see _string_colourmap in
 *               table/palettes.h or docs/ottd-colourtext-palette.png or the enum TextColour in gfx_type.h
 * @param align  The horizontal and vertical alignment of the string.
 * @param underline Whether to underline all strings
 * @param fontsize The size of the initial characters.
 *
 * @return If \a align is #SA_BOTTOM, the top to where we have written, else the bottom to where we have written.
 */
int DrawStringMultiLine(int left, int right, int top, int bottom, StringID str, TextColour colour, StringAlignment align, bool underline, FontSize fontsize)
{
	char buffer[DRAW_STRING_BUFFER];
	GetString(buffer, str, lastof(buffer));
	return DrawStringMultiLine(left, right, top, bottom, buffer, colour, align, underline, fontsize);
}

/**
 * Return the string dimension in pixels. The height and width are returned
 * in a single Dimension value. TINYFONT, BIGFONT modifiers are only
 * supported as the first character of the string. The returned dimensions
 * are therefore a rough estimation correct for all the current strings
 * but not every possible combination
 * @param str string to calculate pixel-width
 * @param start_fontsize Fontsize to start the text with
 * @return string width and height in pixels
 */
Dimension GetStringBoundingBox(const char *str, FontSize start_fontsize)
{
	Layouter layout(str, INT32_MAX, TC_FROMSTRING, start_fontsize);
	return layout.GetBounds();
}

/**
 * Return the string dimension in pixels. The height and width are returned
 * in a single Dimension value. TINYFONT, BIGFONT modifiers are only
 * supported as the first character of the string. The returned dimensions
 * are therefore a rough estimation correct for all the current strings
 * but not every possible combination
 * @param str string to calculate pixel-width
 * @param start_fontsize Fontsize to start the text with
 * @return string width and height in pixels
 */
Dimension GetStringBoundingBox(const std::string &str, FontSize start_fontsize)
{
	return GetStringBoundingBox(str.c_str(), start_fontsize);
}

/**
 * Get bounding box of a string. Uses parameters set by #SetDParam if needed.
 * Has the same restrictions as #GetStringBoundingBox(const char *str, FontSize start_fontsize).
 * @param strid String to examine.
 * @return Width and height of the bounding box for the string in pixels.
 */
Dimension GetStringBoundingBox(StringID strid, FontSize start_fontsize)
{
	char buffer[DRAW_STRING_BUFFER];

	GetString(buffer, strid, lastof(buffer));
	return GetStringBoundingBox(buffer, start_fontsize);
}

/**
 * Get maximum width of a list of strings.
 * @param list List of strings, terminated with INVALID_STRING_ID.
 * @param fontsize Font size to use.
 * @return Width of longest string within the list.
 */
uint GetStringListWidth(const StringID *list, FontSize fontsize)
{
	uint width = 0;
	for (const StringID *str = list; *str != INVALID_STRING_ID; str++) {
		width = std::max(width, GetStringBoundingBox(*str, fontsize).width);
	}
	return width;
}

/**
 * Get the leading corner of a character in a single-line string relative
 * to the start of the string.
 * @param str String containing the character.
 * @param ch Pointer to the character in the string.
 * @param start_fontsize Font size to start the text with.
 * @return Upper left corner of the glyph associated with the character.
 */
Point GetCharPosInString(const char *str, const char *ch, FontSize start_fontsize)
{
	Layouter layout(str, INT32_MAX, TC_FROMSTRING, start_fontsize);
	return layout.GetCharPosition(ch);
}

/**
 * Get the character from a string that is drawn at a specific position.
 * @param str String to test.
 * @param x Position relative to the start of the string.
 * @param start_fontsize Font size to start the text with.
 * @return Pointer to the character at the position or nullptr if there is no character at the position.
 */
const char *GetCharAtPosition(const char *str, int x, FontSize start_fontsize)
{
	if (x < 0) return nullptr;

	Layouter layout(str, INT32_MAX, TC_FROMSTRING, start_fontsize);
	return layout.GetCharAtPosition(x);
}

/**
 * Draw single character horizontally centered around (x,y)
 * @param c           Character (glyph) to draw
 * @param r           Rectangle to draw character within
 * @param colour      Colour to use, for details see _string_colourmap in
 *                    table/palettes.h or docs/ottd-colourtext-palette.png or the enum TextColour in gfx_type.h
 */
void DrawCharCentered(WChar c, const Rect &r, TextColour colour)
{
	GfxBlitterCtx ctx(_cur_dpi);
	ctx.SetColourRemap(colour);
	GfxMainBlitter(ctx, GetGlyph(FS_NORMAL, c),
		CenterBounds(r.left, r.right, GetCharacterWidth(FS_NORMAL, c)),
		CenterBounds(r.top, r.bottom, FONT_HEIGHT_NORMAL),
		BM_COLOUR_REMAP);
}

/**
 * Get the size of a sprite.
 * @param sprid Sprite to examine.
 * @param[out] offset Optionally returns the sprite position offset.
 * @param zoom The zoom level applicable to the sprite.
 * @return Sprite size in pixels.
 * @note The size assumes (0, 0) as top-left coordinate and ignores any part of the sprite drawn at the left or above that position.
 */
Dimension GetSpriteSize(SpriteID sprid, Point *offset, ZoomLevel zoom)
{
	const Sprite *sprite = GetSprite(sprid, SpriteType::Normal);

	if (offset != nullptr) {
		offset->x = UnScaleByZoom(sprite->x_offs, zoom);
		offset->y = UnScaleByZoom(sprite->y_offs, zoom);
	}

	Dimension d;
	d.width  = std::max<int>(0, UnScaleByZoom(sprite->x_offs + sprite->width, zoom));
	d.height = std::max<int>(0, UnScaleByZoom(sprite->y_offs + sprite->height, zoom));
	return d;
}

/**
 * Helper function to get the blitter mode for different types of palettes.
 * @param pal The palette to get the blitter mode for.
 * @return The blitter mode associated with the palette.
 */
static BlitterMode GetBlitterMode(PaletteID pal)
{
	if (HasBit(pal, PALETTE_BRIGHTNESS_MODIFY)) {
		return GB(pal, 0, PALETTE_WIDTH) != PAL_NONE ? BM_COLOUR_REMAP_WITH_BRIGHTNESS : BM_NORMAL_WITH_BRIGHTNESS;
	}
	switch (pal) {
		case PAL_NONE:          return BM_NORMAL;
		case PALETTE_CRASH:     return BM_CRASH_REMAP;
		case PALETTE_ALL_BLACK: return BM_BLACK_REMAP;
		default:                return BM_COLOUR_REMAP;
	}
}

/**
 * Draw a sprite in a viewport.
 * @param dpi  Draw pixel info
 * @param img  Image number to draw
 * @param pal  Palette to use.
 * @param x    Left coordinate of image in viewport, scaled by zoom
 * @param y    Top coordinate of image in viewport, scaled by zoom
 * @param sub  If available, draw only specified part of the sprite
 */
void DrawSpriteViewport(const SpritePointerHolder &sprite_store, const DrawPixelInfo *dpi, SpriteID img, PaletteID pal, int x, int y, const SubSprite *sub)
{
	GfxBlitterCtx ctx(dpi);
	SpriteID real_sprite = GB(img, 0, SPRITE_WIDTH);
	if (HasBit(img, PALETTE_MODIFIER_TRANSPARENT)) {
		ctx.colour_remap_ptr = sprite_store.GetRecolourSprite(GB(pal, 0, PALETTE_WIDTH)) + 1;
		GfxMainBlitterViewport(ctx, sprite_store.GetSprite(real_sprite, SpriteType::Normal), x, y, BM_TRANSPARENT, sub, real_sprite);
	} else if (pal != PAL_NONE) {
		if (HasBit(pal, PALETTE_TEXT_RECOLOUR)) {
			ctx.SetColourRemap((TextColour)GB(pal, 0, PALETTE_WIDTH));
		} else if (GB(pal, 0, PALETTE_WIDTH) != PAL_NONE) {
			ctx.colour_remap_ptr = sprite_store.GetRecolourSprite(GB(pal, 0, PALETTE_WIDTH)) + 1;
		}
		if (HasBit(pal, PALETTE_BRIGHTNESS_MODIFY)) {
			int adjust = GB(pal, PALETTE_BRIGHTNESS_OFFSET, PALETTE_BRIGHTNESS_WIDTH);
			/* Sign extend */
			int sign_bit = 1 << (PALETTE_BRIGHTNESS_WIDTH - 1);
			ctx.sprite_brightness_adjust = (adjust ^ sign_bit) - sign_bit;
		}
		GfxMainBlitterViewport(ctx, sprite_store.GetSprite(real_sprite, SpriteType::Normal), x, y, GetBlitterMode(pal), sub, real_sprite);
	} else {
		GfxMainBlitterViewport(ctx, sprite_store.GetSprite(real_sprite, SpriteType::Normal), x, y, BM_NORMAL, sub, real_sprite);
	}
}

void PrepareDrawSpriteViewportSpriteStore(SpritePointerHolder &sprite_store, SpriteID img, PaletteID pal)
{
	SpriteID real_sprite = GB(img, 0, SPRITE_WIDTH);
	sprite_store.CacheSprite(real_sprite, SpriteType::Normal);
	if (HasBit(img, PALETTE_MODIFIER_TRANSPARENT)) {
		sprite_store.CacheSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour);
	} else if (pal != PAL_NONE) {
		if (!HasBit(pal, PALETTE_TEXT_RECOLOUR) && GB(pal, 0, PALETTE_WIDTH) != PAL_NONE) {
			sprite_store.CacheSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour);
		}
	}
}

/**
 * Draw a sprite, not in a viewport
 * @param img  Image number to draw
 * @param pal  Palette to use.
 * @param x    Left coordinate of image in pixels
 * @param y    Top coordinate of image in pixels
 * @param sub  If available, draw only specified part of the sprite
 * @param zoom Zoom level of sprite
 */
void DrawSprite(SpriteID img, PaletteID pal, int x, int y, const SubSprite *sub, ZoomLevel zoom)
{
	GfxBlitterCtx ctx(_cur_dpi);
	SpriteID real_sprite = GB(img, 0, SPRITE_WIDTH);
	if (HasBit(img, PALETTE_MODIFIER_TRANSPARENT)) {
		ctx.colour_remap_ptr = GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1;
		GfxMainBlitter(ctx, GetSprite(real_sprite, SpriteType::Normal), x, y, BM_TRANSPARENT, sub, real_sprite, zoom);
	} else if (pal != PAL_NONE) {
		if (HasBit(pal, PALETTE_TEXT_RECOLOUR)) {
			ctx.SetColourRemap((TextColour)GB(pal, 0, PALETTE_WIDTH));
		} else {
			ctx.colour_remap_ptr = GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1;
		}
		GfxMainBlitter(ctx, GetSprite(real_sprite, SpriteType::Normal), x, y, GetBlitterMode(pal), sub, real_sprite, zoom);
	} else {
		GfxMainBlitter(ctx, GetSprite(real_sprite, SpriteType::Normal), x, y, BM_NORMAL, sub, real_sprite, zoom);
	}
}

/**
 * The code for setting up the blitter mode and sprite information before finally drawing the sprite.
 * @param sprite The sprite to draw.
 * @param x The X location to draw.
 * @param y The Y location to draw.
 * @param mode The settings for the blitter to pass.
 * @param sub Whether to only draw a sub set of the sprite.
 * @param zoom The zoom level at which to draw the sprites.
 * @param dst Optional parameter for a different blitting destination.
 * @tparam ZOOM_BASE The factor required to get the sub sprite information into the right size.
 * @tparam SCALED_XY Whether the X and Y are scaled or unscaled.
 */
template <int ZOOM_BASE, bool SCALED_XY>
static void GfxBlitter(const GfxBlitterCtx &ctx, const Sprite * const sprite, int x, int y, BlitterMode mode, const SubSprite * const sub, SpriteID sprite_id, ZoomLevel zoom)
{
	const DrawPixelInfo *dpi = ctx.dpi;
	Blitter::BlitterParams bp;

	if (SCALED_XY) {
		/* Scale it */
		x = ScaleByZoom(x, zoom);
		y = ScaleByZoom(y, zoom);
	}

	/* Move to the correct offset */
	x += sprite->x_offs;
	y += sprite->y_offs;

	if (sub == nullptr) {
		/* No clipping. */
		if (sprite->width <= 0 || sprite->height <= 0) return;
		bp.skip_left = 0;
		bp.skip_top = 0;
		bp.width = UnScaleByZoom(sprite->width, zoom);
		bp.height = UnScaleByZoom(sprite->height, zoom);
	} else {
		/* Amount of pixels to clip from the source sprite */
		int clip_left   = std::max(0,                   -sprite->x_offs +  sub->left        * ZOOM_BASE );
		int clip_top    = std::max(0,                   -sprite->y_offs +  sub->top         * ZOOM_BASE );
		int clip_right  = std::max(0, sprite->width  - (-sprite->x_offs + (sub->right + 1)  * ZOOM_BASE));
		int clip_bottom = std::max(0, sprite->height - (-sprite->y_offs + (sub->bottom + 1) * ZOOM_BASE));

		if (clip_left + clip_right >= sprite->width) return;
		if (clip_top + clip_bottom >= sprite->height) return;

		bp.skip_left = UnScaleByZoomLower(clip_left, zoom);
		bp.skip_top = UnScaleByZoomLower(clip_top, zoom);
		bp.width = UnScaleByZoom(sprite->width - clip_left - clip_right, zoom);
		bp.height = UnScaleByZoom(sprite->height - clip_top - clip_bottom, zoom);

		x += ScaleByZoom(bp.skip_left, zoom);
		y += ScaleByZoom(bp.skip_top, zoom);
	}

	/* Copy the main data directly from the sprite */
	bp.sprite = sprite->data;
	bp.sprite_width = sprite->width;
	bp.sprite_height = sprite->height;
	bp.top = 0;
	bp.left = 0;

	bp.dst = dpi->dst_ptr;
	bp.pitch = dpi->pitch;
	bp.remap = ctx.colour_remap_ptr;
	bp.brightness_adjust = ctx.sprite_brightness_adjust;

	if (bp.width <= 0) return;
	if (bp.height <= 0) return;

	y -= SCALED_XY ? ScaleByZoom(dpi->top, zoom) : dpi->top;
	int y_unscaled = UnScaleByZoom(y, zoom);
	/* Check for top overflow */
	if (y < 0) {
		bp.height -= -y_unscaled;
		if (bp.height <= 0) return;
		bp.skip_top += -y_unscaled;
		y = 0;
	} else {
		bp.top = y_unscaled;
	}

	/* Check for bottom overflow */
	y += SCALED_XY ? ScaleByZoom(bp.height - dpi->height, zoom) : ScaleByZoom(bp.height, zoom) - dpi->height;
	if (y > 0) {
		bp.height -= UnScaleByZoom(y, zoom);
		if (bp.height <= 0) return;
	}

	x -= SCALED_XY ? ScaleByZoom(dpi->left, zoom) : dpi->left;
	int x_unscaled = UnScaleByZoom(x, zoom);
	/* Check for left overflow */
	if (x < 0) {
		bp.width -= -x_unscaled;
		if (bp.width <= 0) return;
		bp.skip_left += -x_unscaled;
		x = 0;
	} else {
		bp.left = x_unscaled;
	}

	/* Check for right overflow */
	x += SCALED_XY ? ScaleByZoom(bp.width - dpi->width, zoom) : ScaleByZoom(bp.width, zoom) - dpi->width;
	if (x > 0) {
		bp.width -= UnScaleByZoom(x, zoom);
		if (bp.width <= 0) return;
	}

	assert(bp.skip_left + bp.width <= UnScaleByZoom(sprite->width, zoom));
	assert(bp.skip_top + bp.height <= UnScaleByZoom(sprite->height, zoom));

	/* We do not want to catch the mouse. However we also use that spritenumber for unknown (text) sprites. */
	if (_newgrf_debug_sprite_picker.mode == SPM_REDRAW && sprite_id != SPR_CURSOR_MOUSE) {
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		void *topleft = blitter->MoveTo(bp.dst, bp.left, bp.top);
		void *bottomright = blitter->MoveTo(topleft, bp.width - 1, bp.height - 1);

		void *clicked = _newgrf_debug_sprite_picker.clicked_pixel;

		if (topleft <= clicked && clicked <= bottomright) {
			uint offset = (((size_t)clicked - (size_t)topleft) / (blitter->GetScreenDepth() / 8)) % bp.pitch;
			if (offset < (uint)bp.width) {
				include(_newgrf_debug_sprite_picker.sprites, sprite_id);
			}
		}
	}

	BlitterFactory::GetCurrentBlitter()->Draw(&bp, mode, zoom);
}

/**
 * Draws a sprite to a new RGBA buffer (see Colour union) instead of drawing to the screen.
 *
 * @param spriteId The sprite to draw.
 * @param zoom The zoom level at which to draw the sprites.
 * @return Pixel buffer, or nullptr if an 8bpp blitter is being used.
 */
std::unique_ptr<uint32[]> DrawSpriteToRgbaBuffer(SpriteID spriteId, ZoomLevel zoom)
{
	/* Invalid zoom level requested? */
	if (zoom < _settings_client.gui.zoom_min || zoom > _settings_client.gui.zoom_max) return nullptr;

	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	if (blitter->GetScreenDepth() != 8 && blitter->GetScreenDepth() != 32) return nullptr;

	/* Gather information about the sprite to write, reserve memory */
	const SpriteID real_sprite = GB(spriteId, 0, SPRITE_WIDTH);
	const Sprite *sprite = GetSprite(real_sprite, SpriteType::Normal);
	Dimension dim = GetSpriteSize(real_sprite, nullptr, zoom);
	size_t dim_size = static_cast<size_t>(dim.width) * dim.height;
	std::unique_ptr<uint32[]> result(new uint32[dim_size]);
	/* Set buffer to fully transparent. */
	MemSetT(result.get(), 0, dim_size);

	/* Prepare new DrawPixelInfo - Normally this would be the screen but we want to draw to another buffer here.
	 * Normally, pitch would be scaled screen width, but in our case our "screen" is only the sprite width wide. */
	DrawPixelInfo dpi;
	dpi.dst_ptr = result.get();
	dpi.pitch = dim.width;
	dpi.left = 0;
	dpi.top = 0;
	dpi.width = dim.width;
	dpi.height = dim.height;
	dpi.zoom = zoom;

	dim_size = static_cast<size_t>(dim.width) * dim.height;

	/* If the current blitter is a paletted blitter, we have to render to an extra buffer and resolve the palette later. */
	std::unique_ptr<byte[]> pal_buffer{};
	if (blitter->GetScreenDepth() == 8) {
		pal_buffer.reset(new byte[dim_size]);
		MemSetT(pal_buffer.get(), 0, dim_size);

		dpi.dst_ptr = pal_buffer.get();
	}

	/* Temporarily disable screen animations while blitting - This prevents 40bpp_anim from writing to the animation buffer. */
	Backup<bool> disable_anim(_screen_disable_anim, true, FILE_LINE);
	GfxBlitterCtx ctx(&dpi);
	GfxBlitter<1, true>(ctx, sprite, 0, 0, BM_NORMAL, nullptr, real_sprite, zoom);
	disable_anim.Restore();

	if (blitter->GetScreenDepth() == 8) {
		/* Resolve palette. */
		uint32 *dst = result.get();
		const byte *src = pal_buffer.get();
		for (size_t i = 0; i < dim_size; ++i) {
			*dst++ = _cur_palette.palette[*src++].data;
		}
	}

	return result;
}

static void GfxMainBlitterViewport(const GfxBlitterCtx &ctx, const Sprite *sprite, int x, int y, BlitterMode mode, const SubSprite *sub, SpriteID sprite_id)
{
	GfxBlitter<ZOOM_LVL_BASE, false>(ctx, sprite, x, y, mode, sub, sprite_id, ctx.dpi->zoom);
}

static void GfxMainBlitter(const GfxBlitterCtx &ctx, const Sprite *sprite, int x, int y, BlitterMode mode, const SubSprite *sub, SpriteID sprite_id, ZoomLevel zoom)
{
	GfxBlitter<1, true>(ctx, sprite, x, y, mode, sub, sprite_id, zoom);
}

void DoPaletteAnimations();

Colour _water_palette[10];

void GfxInitPalettes()
{
	MemCpyT<Colour>(_water_palette, (_settings_game.game_creation.landscape == LT_TOYLAND) ? _extra_palette_values.dark_water_toyland : _extra_palette_values.dark_water, 5);
	const Colour *s = (_settings_game.game_creation.landscape == LT_TOYLAND) ? _extra_palette_values.glitter_water_toyland : _extra_palette_values.glitter_water;
	for (int i = 0; i < 5; i++) {
		_water_palette[i + 5] = s[i * 3];
	}

	std::lock_guard<std::mutex> lock_state(_cur_palette_mutex);
	memcpy(&_cur_palette, &_palette, sizeof(_cur_palette));
	DoPaletteAnimations();
}

#define EXTR(p, q) (((uint16)(palette_animation_counter * (p)) * (q)) >> 16)
#define EXTR2(p, q) (((uint16)(~palette_animation_counter * (p)) * (q)) >> 16)

void DoPaletteAnimations()
{
	/* Animation counter for the palette animation. */
	static int palette_animation_counter = 0;
	palette_animation_counter += 8;

	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	const Colour *s;
	const ExtraPaletteValues *ev = &_extra_palette_values;
	Colour old_val[PALETTE_ANIM_SIZE];
	const uint old_tc = palette_animation_counter;
	uint j;

	if (blitter != nullptr && blitter->UsePaletteAnimation() == Blitter::PALETTE_ANIMATION_NONE) {
		palette_animation_counter = 0;
	}

	Colour *palette_pos = &_cur_palette.palette[PALETTE_ANIM_START];  // Points to where animations are taking place on the palette
	/* Makes a copy of the current animation palette in old_val,
	 * so the work on the current palette could be compared, see if there has been any changes */
	memcpy(old_val, palette_pos, sizeof(old_val));

	/* Fizzy Drink bubbles animation */
	s = ev->fizzy_drink;
	j = EXTR2(512, EPV_CYCLES_FIZZY_DRINK);
	for (uint i = 0; i != EPV_CYCLES_FIZZY_DRINK; i++) {
		*palette_pos++ = s[j];
		j++;
		if (j == EPV_CYCLES_FIZZY_DRINK) j = 0;
	}

	/* Oil refinery fire animation */
	s = ev->oil_refinery;
	j = EXTR2(512, EPV_CYCLES_OIL_REFINERY);
	for (uint i = 0; i != EPV_CYCLES_OIL_REFINERY; i++) {
		*palette_pos++ = s[j];
		j++;
		if (j == EPV_CYCLES_OIL_REFINERY) j = 0;
	}

	/* Radio tower blinking */
	{
		byte i = (palette_animation_counter >> 1) & 0x7F;
		byte v;

		if (i < 0x3f) {
			v = 255;
		} else if (i < 0x4A || i >= 0x75) {
			v = 128;
		} else {
			v = 20;
		}
		palette_pos->r = v;
		palette_pos->g = 0;
		palette_pos->b = 0;
		palette_pos++;

		i ^= 0x40;
		if (i < 0x3f) {
			v = 255;
		} else if (i < 0x4A || i >= 0x75) {
			v = 128;
		} else {
			v = 20;
		}
		palette_pos->r = v;
		palette_pos->g = 0;
		palette_pos->b = 0;
		palette_pos++;
	}

	/* Handle lighthouse and stadium animation */
	s = ev->lighthouse;
	j = EXTR(256, EPV_CYCLES_LIGHTHOUSE);
	for (uint i = 0; i != EPV_CYCLES_LIGHTHOUSE; i++) {
		*palette_pos++ = s[j];
		j++;
		if (j == EPV_CYCLES_LIGHTHOUSE) j = 0;
	}

	/* Dark blue water */
	s = (_settings_game.game_creation.landscape == LT_TOYLAND) ? ev->dark_water_toyland : ev->dark_water;
	j = EXTR(320, EPV_CYCLES_DARK_WATER);
	for (uint i = 0; i != EPV_CYCLES_DARK_WATER; i++) {
		*palette_pos++ = s[j];
		j++;
		if (j == EPV_CYCLES_DARK_WATER) j = 0;
	}

	/* Glittery water */
	s = (_settings_game.game_creation.landscape == LT_TOYLAND) ? ev->glitter_water_toyland : ev->glitter_water;
	j = EXTR(128, EPV_CYCLES_GLITTER_WATER);
	for (uint i = 0; i != EPV_CYCLES_GLITTER_WATER / 3; i++) {
		*palette_pos++ = s[j];
		j += 3;
		if (j >= EPV_CYCLES_GLITTER_WATER) j -= EPV_CYCLES_GLITTER_WATER;
	}

	if (blitter != nullptr && blitter->UsePaletteAnimation() == Blitter::PALETTE_ANIMATION_NONE) {
		palette_animation_counter = old_tc;
	} else {
		if (memcmp(old_val, &_cur_palette.palette[PALETTE_ANIM_START], sizeof(old_val)) != 0 && _cur_palette.count_dirty == 0) {
			/* Did we changed anything on the palette? Seems so.  Mark it as dirty */
			_cur_palette.first_dirty = PALETTE_ANIM_START;
			_cur_palette.count_dirty = PALETTE_ANIM_SIZE;
		}
	}
}

/**
 * Determine a contrasty text colour for a coloured background.
 * @param background Background colour.
 * @param threshold Background colour brightness threshold below which the background is considered dark and TC_WHITE is returned, range: 0 - 255, default 128.
 * @return TC_BLACK or TC_WHITE depending on what gives a better contrast.
 */
TextColour GetContrastColour(uint8 background, uint8 threshold)
{
	Colour c = _cur_palette.palette[background];
	/* Compute brightness according to http://www.w3.org/TR/AERT#color-contrast.
	 * The following formula computes 1000 * brightness^2, with brightness being in range 0 to 255. */
	uint sq1000_brightness = c.r * c.r * 299 + c.g * c.g * 587 + c.b * c.b * 114;
	/* Compare with threshold brightness which defaults to 128 (50%) */
	return sq1000_brightness < ((uint) threshold) * ((uint) threshold) * 1000 ? TC_WHITE : TC_BLACK;
}

/**
 * Initialize _stringwidth_table cache
 * @param monospace Whether to load the monospace cache or the normal fonts.
 */
void LoadStringWidthTable(bool monospace)
{
	ClearFontCache();

	for (FontSize fs = monospace ? FS_MONO : FS_BEGIN; fs < (monospace ? FS_END : FS_MONO); fs++) {
		for (uint i = 0; i != 224; i++) {
			_stringwidth_table[fs][i] = GetGlyphWidth(fs, i + 32);
		}
	}
}

/**
 * Return width of character glyph.
 * @param size  Font of the character
 * @param key   Character code glyph
 * @return Width of the character glyph
 */
byte GetCharacterWidth(FontSize size, WChar key)
{
	/* Use _stringwidth_table cache if possible */
	if (key >= 32 && key < 256) return _stringwidth_table[size][key - 32];

	return GetGlyphWidth(size, key);
}

/**
 * Return the maximum width of single digit.
 * @param size  Font of the digit
 * @return Width of the digit.
 */
byte GetDigitWidth(FontSize size)
{
	byte width = 0;
	for (char c = '0'; c <= '9'; c++) {
		width = std::max(GetCharacterWidth(size, c), width);
	}
	return width;
}

/**
 * Determine the broadest digits for guessing the maximum width of a n-digit number.
 * @param[out] front Broadest digit, which is not 0. (Use this digit as first digit for numbers with more than one digit.)
 * @param[out] next Broadest digit, including 0. (Use this digit for all digits, except the first one; or for numbers with only one digit.)
 * @param size  Font of the digit
 */
void GetBroadestDigit(uint *front, uint *next, FontSize size)
{
	int width = -1;
	for (char c = '9'; c >= '0'; c--) {
		int w = GetCharacterWidth(size, c);
		if (w > width) {
			width = w;
			*next = c - '0';
			if (c != '0') *front = c - '0';
		}
	}
}

void ScreenSizeChanged()
{
	MarkWholeScreenDirty();

	/* screen size changed and the old bitmap is invalid now, so we don't want to undraw it */
	_cursor.visible = false;
}

void UndrawMouseCursor()
{
	/* Don't undraw mouse cursor if it is handled by the video driver. */
	if (VideoDriver::GetInstance()->UseSystemCursor()) return;

	/* Don't undraw the mouse cursor if the screen is not ready */
	if (_screen.dst_ptr == nullptr) return;

	if (_cursor.visible) {
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		_cursor.visible = false;
		blitter->CopyFromBuffer(blitter->MoveTo(_screen.dst_ptr, _cursor.draw_pos.x, _cursor.draw_pos.y), _cursor_backup.GetBuffer(), _cursor.draw_size.x, _cursor.draw_size.y);
		VideoDriver::GetInstance()->MakeDirty(_cursor.draw_pos.x, _cursor.draw_pos.y, _cursor.draw_size.x, _cursor.draw_size.y);
	}
}

void DrawMouseCursor()
{
	/* Don't draw mouse cursor if it is handled by the video driver. */
	if (VideoDriver::GetInstance()->UseSystemCursor()) return;

	/* Don't draw the mouse cursor if the screen is not ready */
	if (_screen.dst_ptr == nullptr) return;

	Blitter *blitter = BlitterFactory::GetCurrentBlitter();

	/* Redraw mouse cursor but only when it's inside the window */
	if (!_cursor.in_window) return;

	/* Don't draw the mouse cursor if it's already drawn */
	if (_cursor.visible) {
		if (!_cursor.dirty) return;
		UndrawMouseCursor();
	}

	/* Determine visible area */
	int left = _cursor.pos.x + _cursor.total_offs.x;
	int width = _cursor.total_size.x;
	if (left < 0) {
		width += left;
		left = 0;
	}
	if (left + width > _screen.width) {
		width = _screen.width - left;
	}
	if (width <= 0) return;

	int top = _cursor.pos.y + _cursor.total_offs.y;
	int height = _cursor.total_size.y;
	if (top < 0) {
		height += top;
		top = 0;
	}
	if (top + height > _screen.height) {
		height = _screen.height - top;
	}
	if (height <= 0) return;

	_cursor.draw_pos.x = left;
	_cursor.draw_pos.y = top;
	_cursor.draw_size.x = width;
	_cursor.draw_size.y = height;

	uint8 *buffer = _cursor_backup.Allocate(blitter->BufferSize(_cursor.draw_size.x, _cursor.draw_size.y));

	/* Make backup of stuff below cursor */
	blitter->CopyToBuffer(blitter->MoveTo(_screen.dst_ptr, _cursor.draw_pos.x, _cursor.draw_pos.y), buffer, _cursor.draw_size.x, _cursor.draw_size.y);

	/* Draw cursor on screen */
	_cur_dpi = &_screen;
	for (uint i = 0; i < _cursor.sprite_count; ++i) {
		DrawSprite(_cursor.sprite_seq[i].sprite, _cursor.sprite_seq[i].pal, _cursor.pos.x + _cursor.sprite_pos[i].x, _cursor.pos.y + _cursor.sprite_pos[i].y);
	}

	VideoDriver::GetInstance()->MakeDirty(_cursor.draw_pos.x, _cursor.draw_pos.y, _cursor.draw_size.x, _cursor.draw_size.y);

	_cursor.visible = true;
	_cursor.dirty = false;
}

/**
 * Repaints a specific rectangle of the screen.
 *
 * @param left,top,right,bottom The area of the screen that needs repainting
 * @pre The rectangle is assumed to have been previously marked dirty with \c SetDirtyBlocks.
 * @see SetDirtyBlocks
 * @see DrawDirtyBlocks
 * @ingroup dirty
 *
 */
void RedrawScreenRect(int left, int top, int right, int bottom)
{
	assert(right <= _screen.width && bottom <= _screen.height);
	if (_cursor.visible) {
		if (right > _cursor.draw_pos.x &&
				left < _cursor.draw_pos.x + _cursor.draw_size.x &&
				bottom > _cursor.draw_pos.y &&
				top < _cursor.draw_pos.y + _cursor.draw_size.y) {
			UndrawMouseCursor();
		}
	}

	if (_networking) NetworkUndrawChatMessage();

	DrawOverlappedWindowForAll(left, top, right, bottom);

	VideoDriver::GetInstance()->MakeDirty(left, top, right - left, bottom - top);
}

static std::vector<Rect> _dirty_viewport_occlusions;
static Viewport *_dirty_viewport;
static NWidgetDisplay _dirty_viewport_disp_flags;

static void DrawDirtyViewport(uint occlusion, int left, int top, int right, int bottom)
{
	for(; occlusion < _dirty_viewport_occlusions.size(); occlusion++) {
		const Rect &occ = _dirty_viewport_occlusions[occlusion];
		if (right > occ.left &&
				bottom > occ.top &&
				left < occ.right &&
				top < occ.bottom) {
			/* occlusion and draw rectangle intersect with each other */
			int x;

			if (left < (x = occ.left)) {
				DrawDirtyViewport(occlusion + 1, left, top, x, bottom);
				DrawDirtyViewport(occlusion, x, top, right, bottom);
				return;
			}

			if (right > (x = occ.right)) {
				DrawDirtyViewport(occlusion, left, top, x, bottom);
				DrawDirtyViewport(occlusion + 1, x, top, right, bottom);
				return;
			}

			if (top < (x = occ.top)) {
				DrawDirtyViewport(occlusion + 1, left, top, right, x);
				DrawDirtyViewport(occlusion, left, x, right, bottom);
				return;
			}

			if (bottom > (x = occ.bottom)) {
				DrawDirtyViewport(occlusion, left, top, right, x);
				DrawDirtyViewport(occlusion + 1, left, x, right, bottom);
				return;
			}

			return;
		}
	}

	if (_game_mode == GM_MENU) {
		RedrawScreenRect(left, top, right, bottom);
	} else {
		extern void ViewportDrawChk(Viewport *vp, int left, int top, int right, int bottom, uint8 display_flags);
		ViewportDrawChk(_dirty_viewport, left, top, right, bottom, _dirty_viewport_disp_flags);
		VideoDriver::GetInstance()->MakeDirty(left, top, right - left, bottom - top);
	}
}

static void DrawOverlappedWindowWithClipping(Window *w, int left, int top, int right, int bottom, DrawOverlappedWindowFlags flags)
{
	extern void DrawOverlappedWindow(Window *w, int left, int top, int right, int bottom, DrawOverlappedWindowFlags flags);

	if (right < 0 || bottom < 0 || left >= _screen.width || top >= _screen.height) return;

	DrawOverlappedWindow(w, std::max(0, left), std::max(0, top), std::min(_screen.width, right), std::min(_screen.height, bottom), flags);
}

/**
 * Repaints the rectangle blocks which are marked as 'dirty'.
 *
 * @see SetDirtyBlocks
 *
 * @ingroup dirty
 */
void DrawDirtyBlocks()
{
	static std::vector<NWidgetBase *> dirty_widgets;

	extern void ViewportPrepareVehicleRoute();
	ViewportPrepareVehicleRoute();

	_gfx_draw_active = true;

	if (_whole_screen_dirty) {
		RedrawScreenRect(0, 0, _screen.width, _screen.height);
		for (Window *w : Window::IterateFromBack()) {
			w->flags &= ~(WF_DIRTY | WF_WIDGETS_DIRTY | WF_DRAG_DIRTIED);
		}
		_whole_screen_dirty = false;
	} else {
		bool cleared_overlays = false;
		auto clear_overlays = [&]() {
			if (cleared_overlays) return;
			if (_cursor.visible) UndrawMouseCursor();
			if (_networking) NetworkUndrawChatMessage();
			cleared_overlays = true;
		};

		DrawPixelInfo bk;
		Backup dpi_backup(_cur_dpi, &bk, FILE_LINE);

		for (Window *w : Window::IterateFromBack()) {
			w->flags &= ~WF_DRAG_DIRTIED;
			if (!MayBeShown(w)) continue;

			if (w->viewport != nullptr) w->viewport->is_drawn = false;

			if (w->flags & WF_DIRTY) {
				clear_overlays();
				DrawOverlappedWindowFlags flags = DOWF_MARK_DIRTY;
				if (unlikely(HasBit(_gfx_debug_flags, GDF_SHOW_WINDOW_DIRTY))) {
					flags |= DOWF_SHOW_DEBUG;
				}
				DrawOverlappedWindowWithClipping(w, w->left, w->top, w->left + w->width, w->top + w->height, flags);
				w->flags &= ~(WF_DIRTY | WF_WIDGETS_DIRTY);
			} else if (w->flags & WF_WIDGETS_DIRTY) {
				if (w->nested_root != nullptr) {
					clear_overlays();
					w->nested_root->FillDirtyWidgets(dirty_widgets);
					for (NWidgetBase *widget : dirty_widgets) {
						DrawOverlappedWindowFlags flags = DOWF_MARK_DIRTY;
						if (unlikely(HasBit(_gfx_debug_flags, GDF_SHOW_WIDGET_DIRTY))) {
							flags |= DOWF_SHOW_DEBUG;
						}
						DrawOverlappedWindowWithClipping(w, w->left + widget->pos_x, w->top + widget->pos_y, w->left + widget->pos_x + widget->current_x, w->top + widget->pos_y + widget->current_y, flags);
					}
					dirty_widgets.clear();
				}
				w->flags &= ~WF_WIDGETS_DIRTY;
			}

			if (w->viewport != nullptr && !w->IsShaded()) {
				Viewport *vp = w->viewport;
				if (vp->is_drawn) {
					vp->ClearDirty();
				} else if (vp->is_dirty) {
					clear_overlays();
					PerformanceAccumulator framerate(PFE_DRAWWORLD);
					_cur_dpi->left = 0;
					_cur_dpi->top = 0;
					_cur_dpi->width = _screen.width;
					_cur_dpi->height = _screen.height;
					_cur_dpi->pitch = _screen.pitch;
					_cur_dpi->dst_ptr = _screen.dst_ptr;
					_cur_dpi->zoom = ZOOM_LVL_NORMAL;

					_dirty_viewport = vp;
					_dirty_viewport_disp_flags = w->viewport_widget->disp_flags;
					TransparencyOptionBits to_backup = _transparency_opt;
					if (_dirty_viewport_disp_flags & ND_NO_TRANSPARENCY) {
						_transparency_opt &= (1 << TO_SIGNS) | (1 << TO_LOADING); // Disable all transparency, except textual stuff
					}

					{
						int left = vp->left;
						int top = vp->top;
						int right = vp->left + vp->width;
						int bottom = vp->top + vp->height;
						_dirty_viewport_occlusions.clear();
						for (const Window *v : Window::IterateFromBack(w->z_front)) {
							if (MayBeShown(v) &&
									right > v->left &&
									bottom > v->top &&
									left < v->left + v->width &&
									top < v->top + v->height) {
								_dirty_viewport_occlusions.push_back({ v->left, v->top, v->left + v->width, v->top + v->height });
							}
						}
						for (const Rect &r : _dirty_blocks) {
							if (right > r.left &&
									bottom > r.top &&
									left < r.right &&
									top < r.bottom) {
								_dirty_viewport_occlusions.push_back({ r.left, r.top, r.right, r.bottom });
							}
						}
					}

					const uint grid_w = vp->dirty_blocks_per_row;
					const uint grid_h = vp->dirty_blocks_per_column;

					uint pos = 0;
					uint x = 0;
					do {
						uint y = 0;
						do {
							if (vp->dirty_blocks[pos]) {
								uint left = x;
								uint top = y;
								uint right = x + 1;
								uint bottom = y;
								uint p = pos;

								/* First try coalescing downwards */
								do {
									vp->dirty_blocks[p] = false;
									p++;
									bottom++;
								} while (bottom != grid_h && vp->dirty_blocks[p]);

								/* Try coalescing to the right too. */
								uint block_h = (bottom - y);
								p = pos;

								while (right != grid_w) {
									uint p2 = (p += grid_h);
									uint check_h = block_h;
									/* Check if a full line of dirty flags is set. */
									do {
										if (!vp->dirty_blocks[p2]) goto no_more_coalesc;
										p2++;
									} while (--check_h != 0);

									/* Wohoo, can combine it one step to the right!
									 * Do that, and clear the bits. */
									right++;

									check_h = block_h;
									p2 = p;
									do {
										vp->dirty_blocks[p2] = false;
										p2++;
									} while (--check_h != 0);
								}
								no_more_coalesc:

								assert(_cur_dpi == &bk);
								int draw_left = std::max<int>(0, ((left == 0) ? 0 : vp->dirty_block_left_margin + (left << vp->GetDirtyBlockWidthShift())) + vp->left);
								int draw_top = std::max<int>(0, (top << vp->GetDirtyBlockHeightShift()) + vp->top);
								int draw_right = std::min<int>(_screen.width, std::min<int>((right << vp->GetDirtyBlockWidthShift()) + vp->dirty_block_left_margin, vp->width) + vp->left);
								int draw_bottom = std::min<int>(_screen.height, std::min<int>(bottom << vp->GetDirtyBlockHeightShift(), vp->height) + vp->top);
								if (draw_left < draw_right && draw_top < draw_bottom) {
									DrawDirtyViewport(0, draw_left, draw_top, draw_right, draw_bottom);
								}
							}
						} while (pos++, ++y != grid_h);
					} while (++x != grid_w);

					_transparency_opt = to_backup;
					w->viewport->ClearDirty();
				}
			}
		}

		dpi_backup.Restore();

		for (const Rect &r : _dirty_blocks) {
			RedrawScreenRect(r.left, r.top, r.right, r.bottom);
		}
		if (unlikely(HasBit(_gfx_debug_flags, GDF_SHOW_RECT_DIRTY))) {
			ViewportDoDrawProcessAllPending();
			for (const Rect &r : _dirty_blocks) {
				GfxFillRect(r.left, r.top, r.right, r.bottom, _string_colourmap[(_dirty_block_colour.fetch_add(1, std::memory_order_relaxed) + 1) & 0xF], FILLRECT_CHECKER);
			}
		}
	}

	_dirty_blocks.clear();
	while (!_pending_dirty_blocks.empty()) {
		for (const Rect &r : _pending_dirty_blocks) {
			SetDirtyBlocks(r.left, r.top, r.right, r.bottom);
		}
		_pending_dirty_blocks.clear();
		for (const Rect &r : _dirty_blocks) {
			RedrawScreenRect(r.left, r.top, r.right, r.bottom);
		}
		_dirty_blocks.clear();
	}
	ViewportDoDrawProcessAllPending();
	_gfx_draw_active = false;
	_dirty_block_colour.fetch_add(1, std::memory_order_relaxed);

	extern void ClearViewportCaches();
	ClearViewportCaches();
}

void UnsetDirtyBlocks(int left, int top, int right, int bottom)
{
	if (_whole_screen_dirty) return;

	for (uint i = 0; i < _dirty_blocks.size(); i++) {
		Rect &r = _dirty_blocks[i];
		if (left < r.right &&
				right > r.left &&
				top < r.bottom &&
				bottom > r.top) {
			/* overlap of some sort */
			if (left <= r.left &&
					right >= r.right &&
					top <= r.top &&
					bottom >= r.bottom) {
				/* dirty rect entirely in subtraction area */
				r = _dirty_blocks.back();
				_dirty_blocks.pop_back();
				i--;
				continue;
			}
			if (r.left < left) {
				Rect n = { left, r.top, r.right, r.bottom };
				r.right = left;
				_dirty_blocks.push_back(n);
				continue;
			}
			if (r.right > right) {
				Rect n = { r.left, r.top, right, r.bottom };
				r.left = right;
				_dirty_blocks.push_back(n);
				continue;
			}
			if (r.top < top) {
				Rect n = { r.left, top, r.right, r.bottom };
				r.bottom = top;
				_dirty_blocks.push_back(n);
				continue;
			}
			if (r.bottom > bottom) {
				Rect n = { r.left, r.top, r.right, bottom };
				r.top = bottom;
				_dirty_blocks.push_back(n);
				continue;
			}
		}
	}
}

static void AddDirtyBlocks(uint start, int left, int top, int right, int bottom)
{
	if (bottom <= top || right <= left) return;

	for (; start < _dirty_blocks.size(); start++) {
		Rect &r = _dirty_blocks[start];
		if (left <= r.right &&
				right >= r.left &&
				top <= r.bottom &&
				bottom >= r.top) {
			/* overlap or contact of some sort */
			if (left >= r.left &&
					right <= r.right &&
					top >= r.top &&
					bottom <= r.bottom) {
				/* entirely contained by existing */
				return;
			}
			if (left <= r.left &&
					right >= r.right &&
					top <= r.top &&
					bottom >= r.bottom) {
				/* entirely contains existing */
				r = _dirty_blocks.back();
				_dirty_blocks.pop_back();
				start--;
				continue;
			}
			if (left < r.left && right > r.left) {
				int middle = r.left;
				AddDirtyBlocks(start, left, top, middle, bottom);
				AddDirtyBlocks(start, middle, top, right, bottom);
				return;
			}
			if (right > r.right && left < r.right) {
				int middle = r.right;
				AddDirtyBlocks(start, left, top, middle, bottom);
				AddDirtyBlocks(start, middle, top, right, bottom);
				return;
			}

			if (top < r.top && bottom > r.top) {
				int middle = r.top;
				AddDirtyBlocks(start, left, top, right, middle);
				AddDirtyBlocks(start, left, middle, right, bottom);
				return;
			}

			if (bottom > r.bottom && top < r.bottom) {
				int middle = r.bottom;
				AddDirtyBlocks(start, left, top, right, middle);
				AddDirtyBlocks(start, left, middle, right, bottom);
				return;
			}
		}
	}
	_dirty_blocks.push_back({ left, top, right, bottom });
}

/**
 * Add the specified rectangle to the collection of screen areas to be
 * invalidated and redrawn.
 * Note the point (0,0) is top left.
 *
 * @param left The left edge of the rectangle
 * @param top The top edge of the rectangle
 * @param right The right edge of the rectangle
 * @param bottom The bottom edge of the rectangle
 * @see DrawDirtyBlocks
 *
 * @ingroup dirty
 */
void SetDirtyBlocks(int left, int top, int right, int bottom)
{
	if (_whole_screen_dirty) return;

	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > _screen.width) right = _screen.width;
	if (bottom > _screen.height) bottom = _screen.height;

	AddDirtyBlocks(0, left, top, right, bottom);
}

void SetPendingDirtyBlocks(int left, int top, int right, int bottom)
{
	_pending_dirty_blocks.push_back({ left, top, right, bottom });
}

/**
 * This function mark the whole screen as dirty. This results in repainting
 * the whole screen. Use this with care as this function will break the
 * idea about marking only parts of the screen as 'dirty'.
 * @ingroup dirty
 */
void MarkWholeScreenDirty()
{
	_whole_screen_dirty = true;
}

/**
 * Set up a clipping area for only drawing into a certain area. To do this,
 * Fill a DrawPixelInfo object with the supplied relative rectangle, backup
 * the original (calling) _cur_dpi and assign the just returned DrawPixelInfo
 * _cur_dpi. When you are done, give restore _cur_dpi's original value
 * @param *n the DrawPixelInfo that will be the clipping rectangle box allowed
 * for drawing
 * @param left,top,width,height the relative coordinates of the clipping
 * rectangle relative to the current _cur_dpi. This will most likely be the
 * offset from the calling window coordinates
 * @return return false if the requested rectangle is not possible with the
 * current dpi pointer. Only continue of the return value is true, or you'll
 * get some nasty results
 */
bool FillDrawPixelInfo(DrawPixelInfo *n, int left, int top, int width, int height)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	const DrawPixelInfo *o = _cur_dpi;

	n->zoom = ZOOM_LVL_NORMAL;

	assert(width > 0);
	assert(height > 0);

	if ((left -= o->left) < 0) {
		width += left;
		if (width <= 0) return false;
		n->left = -left;
		left = 0;
	} else {
		n->left = 0;
	}

	if (width > o->width - left) {
		width = o->width - left;
		if (width <= 0) return false;
	}
	n->width = width;

	if ((top -= o->top) < 0) {
		height += top;
		if (height <= 0) return false;
		n->top = -top;
		top = 0;
	} else {
		n->top = 0;
	}

	n->dst_ptr = blitter->MoveTo(o->dst_ptr, left, top);
	n->pitch = o->pitch;

	if (height > o->height - top) {
		height = o->height - top;
		if (height <= 0) return false;
	}
	n->height = height;

	return true;
}

/**
 * Update cursor dimension.
 * Called when changing cursor sprite resp. reloading grfs.
 */
void UpdateCursorSize()
{
	/* Ignore setting any cursor before the sprites are loaded. */
	if (GetMaxSpriteID() == 0) return;

	static_assert(lengthof(_cursor.sprite_seq) == lengthof(_cursor.sprite_pos));
	assert(_cursor.sprite_count <= lengthof(_cursor.sprite_seq));
	for (uint i = 0; i < _cursor.sprite_count; ++i) {
		const Sprite *p = GetSprite(GB(_cursor.sprite_seq[i].sprite, 0, SPRITE_WIDTH), SpriteType::Normal);
		Point offs, size;
		offs.x = UnScaleGUI(p->x_offs) + _cursor.sprite_pos[i].x;
		offs.y = UnScaleGUI(p->y_offs) + _cursor.sprite_pos[i].y;
		size.x = UnScaleGUI(p->width);
		size.y = UnScaleGUI(p->height);

		if (i == 0) {
			_cursor.total_offs = offs;
			_cursor.total_size = size;
		} else {
			int right  = std::max(_cursor.total_offs.x + _cursor.total_size.x, offs.x + size.x);
			int bottom = std::max(_cursor.total_offs.y + _cursor.total_size.y, offs.y + size.y);
			if (offs.x < _cursor.total_offs.x) _cursor.total_offs.x = offs.x;
			if (offs.y < _cursor.total_offs.y) _cursor.total_offs.y = offs.y;
			_cursor.total_size.x = right  - _cursor.total_offs.x;
			_cursor.total_size.y = bottom - _cursor.total_offs.y;
		}
	}

	_cursor.dirty = true;
}

/**
 * Switch cursor to different sprite.
 * @param cursor Sprite to draw for the cursor.
 * @param pal Palette to use for recolouring.
 */
static void SetCursorSprite(CursorID cursor, PaletteID pal)
{
	if (_cursor.sprite_count == 1 && _cursor.sprite_seq[0].sprite == cursor && _cursor.sprite_seq[0].pal == pal) return;

	_cursor.sprite_count = 1;
	_cursor.sprite_seq[0].sprite = cursor;
	_cursor.sprite_seq[0].pal = pal;
	_cursor.sprite_pos[0].x = 0;
	_cursor.sprite_pos[0].y = 0;

	UpdateCursorSize();
}

static void SwitchAnimatedCursor()
{
	const AnimCursor *cur = _cursor.animate_cur;

	if (cur == nullptr || cur->sprite == AnimCursor::LAST) cur = _cursor.animate_list;

	SetCursorSprite(cur->sprite, _cursor.sprite_seq[0].pal);

	_cursor.animate_timeout = cur->display_time;
	_cursor.animate_cur     = cur + 1;
}

void CursorTick()
{
	if (_cursor.animate_timeout != 0 && --_cursor.animate_timeout == 0) {
		SwitchAnimatedCursor();
	}
}

/**
 * Set or unset the ZZZ cursor.
 * @param busy Whether to show the ZZZ cursor.
 */
void SetMouseCursorBusy(bool busy)
{
	if (busy) {
		if (_cursor.sprite_seq[0].sprite == SPR_CURSOR_MOUSE) SetMouseCursor(SPR_CURSOR_ZZZ, PAL_NONE);
	} else {
		if (_cursor.sprite_seq[0].sprite == SPR_CURSOR_ZZZ) SetMouseCursor(SPR_CURSOR_MOUSE, PAL_NONE);
	}
}

/**
 * Assign a single non-animated sprite to the cursor.
 * @param sprite Sprite to draw for the cursor.
 * @param pal Palette to use for recolouring.
 * @see SetAnimatedMouseCursor
 */
void SetMouseCursor(CursorID sprite, PaletteID pal)
{
	/* Turn off animation */
	_cursor.animate_timeout = 0;
	/* Set cursor */
	SetCursorSprite(sprite, pal);
}

/**
 * Assign an animation to the cursor.
 * @param table Array of animation states.
 * @see SetMouseCursor
 */
void SetAnimatedMouseCursor(const AnimCursor *table)
{
	_cursor.animate_list = table;
	_cursor.animate_cur = nullptr;
	_cursor.sprite_seq[0].pal = PAL_NONE;
	SwitchAnimatedCursor();
}

/**
 * Update cursor position on mouse movement for relative modes.
 * @param delta_x How much change in the X position.
 * @param delta_y How much change in the Y position.
 */
void CursorVars::UpdateCursorPositionRelative(int delta_x, int delta_y)
{
	if (this->fix_at) {
		this->delta.x = delta_x;
		this->delta.y = delta_y;
	} else {
		int last_position_x = this->pos.x;
		int last_position_y = this->pos.y;

		this->pos.x = Clamp(this->pos.x + delta_x, 0, _cur_resolution.width - 1);
		this->pos.y = Clamp(this->pos.y + delta_y, 0, _cur_resolution.height - 1);

		this->delta.x = last_position_x - this->pos.x;
		this->delta.y = last_position_y - this->pos.y;

		this->dirty = true;
	}
}

/**
 * Update cursor position on mouse movement.
 * @param x New X position.
 * @param y New Y position.
 * @param queued_warp True, if the OS queues mouse warps after pending mouse movement events.
 *                    False, if the warp applies instantaneous.
 * @return true, if the OS cursor position should be warped back to this->pos.
 */
bool CursorVars::UpdateCursorPosition(int x, int y, bool queued_warp)
{
	/* Detecting relative mouse movement is somewhat tricky.
	 *  - There may be multiple mouse move events in the video driver queue (esp. when OpenTTD lags a bit).
	 *  - When we request warping the mouse position (return true), a mouse move event is appended at the end of the queue.
	 *
	 * So, when this->fix_at is active, we use the following strategy:
	 *  - The first movement triggers the warp to reset the mouse position.
	 *  - Subsequent events have to compute movement relative to the previous event.
	 *  - The relative movement is finished, when we receive the event matching the warp.
	 */

	if (x == this->pos.x && y == this->pos.y) {
		/* Warp finished. */
		this->queued_warp = false;
	}

	this->delta.x = x - (this->queued_warp ? this->last_position.x : this->pos.x);
	this->delta.y = y - (this->queued_warp ? this->last_position.y : this->pos.y);

	this->last_position.x = x;
	this->last_position.y = y;

	bool need_warp = false;
	if (this->fix_at) {
		if (this->delta.x != 0 || this->delta.y != 0) {
			/* Trigger warp.
			 * Note: We also trigger warping again, if there is already a pending warp.
			 *       This makes it more tolerant about the OS or other software in between
			 *       botchering the warp. */
			this->queued_warp = queued_warp;
			need_warp = true;
		}
	} else if (this->pos.x != x || this->pos.y != y) {
		this->queued_warp = false; // Cancel warping, we are no longer confining the position.
		this->dirty = true;
		this->pos.x = x;
		this->pos.y = y;
	}
	return need_warp;
}

bool ChangeResInGame(int width, int height)
{
	return (_screen.width == width && _screen.height == height) || VideoDriver::GetInstance()->ChangeResolution(width, height);
}

bool ToggleFullScreen(bool fs)
{
	bool result = VideoDriver::GetInstance()->ToggleFullscreen(fs);
	if (_fullscreen != fs && _resolutions.empty()) {
		DEBUG(driver, 0, "Could not find a suitable fullscreen resolution");
	}
	return result;
}

void SortResolutions()
{
	std::sort(_resolutions.begin(), _resolutions.end());
}

/**
 * Resolve GUI zoom level, if auto-suggestion is requested.
 */
void UpdateGUIZoom()
{
	int old_scale = _gui_scale;

	/* Determine real GUI zoom to use. */
	if (_gui_scale_cfg == -1) {
		_gui_scale = VideoDriver::GetInstance()->GetSuggestedUIScale();
	} else {
		_gui_scale = Clamp(_gui_scale_cfg, MIN_INTERFACE_SCALE, MAX_INTERFACE_SCALE);
	}

	int8 new_zoom = ScaleGUITrad(1) <= 1 ? ZOOM_LVL_OUT_4X : ScaleGUITrad(1) >= 4 ? ZOOM_LVL_MIN : ZOOM_LVL_OUT_2X;
	/* Font glyphs should not be clamped to min/max zoom. */
	_font_zoom = static_cast<ZoomLevel>(new_zoom);
	/* Ensure the gui_zoom is clamped between min/max. */
	new_zoom = Clamp(new_zoom, _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);
	_gui_zoom = static_cast<ZoomLevel>(new_zoom);

	if (old_scale != _gui_scale) {
		ClearFontCache();
	}
	UpdateFontHeightCache();
}

/**
 * Resolve GUI zoom level and adjust GUI to new zoom, if auto-suggestion is requested.
 * @param automatic Set if the change is occuring due to OS DPI scaling being changed.
 * @returns true when the zoom level has changed, caller must call ReInitAllWindows(true)
 * after resizing the application's window/buffer.
 */
bool AdjustGUIZoom(AdjustGUIZoomMode mode)
{
	if (mode == AGZM_STARTUP) {
		_adjust_gui_zoom_startup_done = true;
	} else if (!_adjust_gui_zoom_startup_done) {
		return false;
	}

	ZoomLevel old_gui_zoom = _gui_zoom;
	ZoomLevel old_font_zoom = _font_zoom;
	int old_scale = _gui_scale;
	UpdateGUIZoom();
	if (old_scale == _gui_scale) return false;

	/* Reload sprites if sprite zoom level has changed. */
	if (old_gui_zoom != _gui_zoom) {
		GfxClearSpriteCache();
		VideoDriver::GetInstance()->ClearSystemSprites();
		UpdateCursorSize();
		if (mode != AGZM_STARTUP) UpdateRouteStepSpriteSize();
	} else if (old_font_zoom != _font_zoom) {
		GfxClearFontSpriteCache();
	}

	ClearFontCache();
	UpdateFontHeightCache();
	LoadStringWidthTable();
	ReInitAllWindows(false);
	UpdateAllVirtCoords();
	if (mode != AGZM_STARTUP) FixTitleGameZoom();

	extern void FlushDeparturesWindowTextCaches();
	FlushDeparturesWindowTextCaches();

	/* Adjust all window sizes to match the new zoom level, so that they don't appear
	   to move around when the application is moved to a screen with different DPI. */
	auto zoom_shift = old_gui_zoom - _gui_zoom;
	for (Window *w : Window::IterateFromBack()) {
		if (mode == AGZM_AUTOMATIC) {
			w->left   = (w->left   * _gui_scale) / old_scale;
			w->top    = (w->top    * _gui_scale) / old_scale;
			w->width  = (w->width  * _gui_scale) / old_scale;
			w->height = (w->height * _gui_scale) / old_scale;
		}
		if (w->viewport != nullptr) {
			w->viewport->zoom = Clamp(ZoomLevel(w->viewport->zoom - zoom_shift), _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);
		}
	}

	return true;
}

void ChangeGameSpeed(bool enable_fast_forward)
{
	if (enable_fast_forward) {
		_game_speed = _settings_client.gui.fast_forward_speed_limit;
	} else {
		_game_speed = 100;
	}
}

void SetupTickRate()
{
	_milliseconds_per_tick = (_settings_game.economy.tick_rate == TRM_MODERN) ? 27 : 30;
	_ticks_per_second = 1000.0f / _milliseconds_per_tick;
}
