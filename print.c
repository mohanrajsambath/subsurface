#include <glib/gi18n.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <gtk/gtk.h>

#include "dive.h"
#include "display.h"
#include "display-gtk.h"

#define FONT_NORMAL (12)
#define FONT_SMALL (FONT_NORMAL / 1.2)
#define FONT_LARGE (FONT_NORMAL * 1.2)

static struct options print_options;

typedef struct _Print_params {
	int rotation, dives;
	double w_scale, h_scale;
} Print_params;

/* Return the 'i'th dive for printing, taking our dive selection into account */
static struct dive *get_dive_for_printing(int idx)
{
	if (print_options.print_selected) {
		int i;
		struct dive *dive;
		for_each_dive(i, dive) {
			if (!dive->selected)
				continue;
			if (!idx)
				return dive;
			idx--;
		}
		return NULL;
	}
	return get_dive(idx);
}

static void set_font(PangoLayout *layout, PangoFontDescription *font,
	double size, int align)
{
	pango_font_description_set_size(font, size * PANGO_SCALE * SCALE_PRINT);
	pango_layout_set_font_description(layout, font);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(layout, align);
}

/*
 * You know what? Maybe somebody can do a real Pango layout thing.
 * This is hacky.
 */

/*
 * Show a header for the dive containing minimal data
 */
static void show_dive_header(struct dive *dive, cairo_t *cr, double w,
	double h, PangoFontDescription *font, double w_scale_factor)
{
	double depth;
	const char *unit;
	int len, decimals, width, height, maxwidth, maxheight;
	PangoLayout *layout;
	PangoRectangle ink_ext, logic_ext;
	struct tm tm;
	char buffer[160], divenr[40], *people;

	maxwidth = w * PANGO_SCALE;
	maxheight = h * PANGO_SCALE * 0.9;
	cairo_save(cr);
	layout = pango_cairo_create_layout(cr);
	pango_layout_set_width(layout, maxwidth);
	pango_layout_set_height(layout, maxheight);

	*divenr = 0;
	if (dive->number)
		snprintf(divenr, sizeof(divenr), _("Dive #%d - "), dive->number);

	utc_mkdate(dive->when, &tm);
	len = snprintf(buffer, sizeof(buffer),
		/*++GETTEXT 80 chars: lead text ("" or localized "Dive #%d - ") weekday, monthname, day, year, hour, min */
		_("%1$s%2$s, %3$s %4$d, %5$d   %6$d:%7$02d"),
		divenr,
		weekday(tm.tm_wday),
		monthname(tm.tm_mon),
		tm.tm_mday, tm.tm_year + 1900,
		tm.tm_hour, tm.tm_min);
	set_font(layout, font, FONT_LARGE *(1.5/w_scale_factor), PANGO_ALIGN_LEFT);
	pango_layout_set_text(layout, buffer, len);
	pango_layout_get_size(layout, &width, &height);
	cairo_move_to(cr, 0, 0);
	pango_cairo_show_layout(cr, layout);

	people = dive->buddy;
	if (!people || !*people) {
		people = dive->divemaster;
		if (!people)
			people = "";
	}

	depth = get_depth_units(dive->maxdepth.mm, &decimals, &unit);
	snprintf(buffer, sizeof(buffer),
		_("Max depth: %.*f %s\nDuration: %d min\n%s"),
		decimals, depth, unit,
		(dive->duration.seconds+59) / 60,
		people);
	set_font(layout, font, FONT_NORMAL*(1.5/w_scale_factor), PANGO_ALIGN_RIGHT);
	pango_layout_set_text(layout, buffer, -1);
	cairo_move_to(cr, 0, 0);
	pango_cairo_show_layout(cr, layout);

	/*
	 * Show the dive location
	 *
	 * .. or at least a space to get the size.
	 *
	 * Move down by the size of the date, and limit the
	 * width to the same width as the date string.
	 */
	pango_layout_get_extents (layout, &ink_ext, &logic_ext);
	cairo_translate (cr, 0, ink_ext.height /(1.5 * (double) PANGO_SCALE));
	pango_layout_set_height(layout, 1);
	pango_layout_set_width(layout, width);
	set_font(layout, font, FONT_LARGE*(1.5/w_scale_factor), PANGO_ALIGN_LEFT);
	pango_layout_set_text(layout, dive->location ? : " ", -1);
	cairo_move_to(cr, 0, 0);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	cairo_restore(cr);
}
/*
 * Show the dive notes
 */
static void show_dive_notes(struct dive *dive, cairo_t *cr, double w,
	double h, PangoFontDescription *font, double w_scale_factor)
{
	int maxwidth, maxheight;
	PangoLayout *layout;

	maxwidth = w * PANGO_SCALE;
	maxheight = h * PANGO_SCALE * 0.9;
	layout = pango_cairo_create_layout(cr);
	if (dive->notes) {
		pango_layout_set_height(layout, maxheight);
		pango_layout_set_width(layout, maxwidth);
		set_font(layout, font, FONT_NORMAL*(1.5/w_scale_factor), PANGO_ALIGN_LEFT);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
		pango_layout_set_justify(layout, 1);
		pango_layout_set_text(layout, dive->notes, -1);

		cairo_move_to(cr, 0, 0);
		pango_cairo_show_layout(cr, layout);
	}
	g_object_unref(layout);
}
/* Print the used gas mix */
static void print_ean_trimix (cairo_t *cr, PangoLayout *layout, int O2, int He){

	char buffer[64];

	if (He){
		snprintf(buffer, sizeof(buffer), "Tx%d/%d", O2, He);
	}else{
		if (O2){
			if (O2 == 100){
				snprintf(buffer, sizeof(buffer), _("Oxygen"));
			}else{
				snprintf(buffer, sizeof(buffer), "EAN%d", O2);
			}
		}else{
			snprintf(buffer, sizeof(buffer), _("air"));
		}
	}
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
}

static unsigned start_pressure(cylinder_t *cyl)
{
	return cyl->start.mbar ? : cyl->sample_start.mbar;
}

static unsigned end_pressure(cylinder_t *cyl)
{
	return cyl->end.mbar ? : cyl->sample_end.mbar;
}

/* Print the tank data */
static void print_tanks (struct dive *dive, cairo_t *cr, PangoLayout *layout, int maxwidth, int maxheight,
		int tank_count, int first_tank, PangoFontDescription *font,
        double w_scale_factor)
{
	int curwidth, n, i, counter;
	char buffer[80], dataheader1[3][80]= { N_("Cylinder"), N_("Gasmix"), NC_("Amount","Gas Used")};
	PangoRectangle logic_ext;

	cairo_save(cr);

	/* First create a header */
	curwidth = 0;
	pango_layout_get_extents(layout, NULL, &logic_ext);

	for (i = 0; i < 3; i++) {
		cairo_move_to(cr, curwidth / (double) PANGO_SCALE, 0);
		pango_layout_set_text(layout, _(dataheader1[i]), -1);
		pango_layout_set_justify(layout, 0);
		pango_cairo_show_layout(cr, layout);
		curwidth = curwidth + maxwidth/ 3;
	}
	/* Then the cylinder stuff */
	n = first_tank;
	counter = 0;
	pango_layout_get_extents(layout, NULL, &logic_ext);
	cairo_translate (cr, 0, logic_ext.height / (double) PANGO_SCALE);

	while (n < tank_count && n < first_tank + 4) {
		int decimals;
		const char *unit, *desc;
		double gas_usage;
		cylinder_t *cyl = dive->cylinder + n;

		/* Get the cylinder gas use in mbar */
		gas_usage = start_pressure(cyl) - end_pressure(cyl);

		/* Can we turn it into a volume? */
		if (cyl->type.size.mliter) {
			gas_usage = bar_to_atm(gas_usage / 1000);
			gas_usage *= cyl->type.size.mliter;
			gas_usage = get_volume_units(gas_usage, &decimals, &unit);
		} else {
			gas_usage = get_pressure_units(gas_usage, &unit);
			decimals = 0;
		}

		curwidth = 0;
		cairo_move_to (cr, curwidth / (double) PANGO_SCALE, 0);
		desc = cyl->type.description ? : "";
		snprintf(buffer, sizeof(buffer), "%s", desc);
		pango_layout_set_text(layout, buffer, -1);
		pango_cairo_show_layout(cr, layout);
		curwidth += (maxwidth/ 3);

		cairo_move_to(cr, curwidth / (double) PANGO_SCALE, 0);
		print_ean_trimix (cr, layout,
				cyl->gasmix.o2.permille/10,
				cyl->gasmix.he.permille/10);
		curwidth += (maxwidth/ 3);

		cairo_move_to(cr, curwidth / (double) PANGO_SCALE, 0);
		snprintf(buffer, sizeof(buffer), _("%.*f %s\n"),
			decimals,
			gas_usage,
			unit);
		pango_layout_set_text(layout, buffer, -1);
		pango_cairo_show_layout(cr, layout);
		curwidth += (maxwidth/ 3);
		n++;
		counter++;
		pango_layout_get_extents(layout, NULL, &logic_ext);
		cairo_translate (cr, 0, logic_ext.height / (2*(double) PANGO_SCALE));
	}
	g_object_unref (layout);
	cairo_restore(cr);
}

/* Print weight system */
static void print_weight_data (struct dive *dive, cairo_t *cr, int maxwidth, int maxheight,
		PangoFontDescription *font, double w_scale_factor)
{
	int decimals,i;
	double totalweight, systemweight, weightsystemcounter;
	const char *unit_weight, *desc;
	char buffer[80];
	PangoLayout *layout;
	PangoRectangle ink_extents, logical_extents;

	cairo_save(cr);

	layout = pango_cairo_create_layout(cr);
	pango_layout_set_height(layout,maxheight);
	pango_layout_set_width(layout, maxwidth);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	set_font(layout, font, FONT_SMALL*(1.5/w_scale_factor), PANGO_ALIGN_CENTER);

	/* Header for the weight system */
	cairo_move_to(cr, 0, 0);
	snprintf (buffer, sizeof(buffer),_("Weight System"));
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);

	/* Detail of the weight */
	pango_layout_get_extents(layout, &ink_extents, &logical_extents);
	cairo_translate (cr, 0, logical_extents.height / (double) PANGO_SCALE);
	set_font(layout, font, FONT_SMALL*(1.5/w_scale_factor), PANGO_ALIGN_LEFT);
	weightsystemcounter = 0;
	for (i=0; i<  MAX_WEIGHTSYSTEMS; i++){
		systemweight = get_weight_units(dive->weightsystem[i].weight.grams, &decimals, &unit_weight);
		if (systemweight != 0){
			cairo_move_to(cr, 0, 0);
			desc = dive->weightsystem[i].description ? : _("unknown");
			snprintf(buffer, sizeof(buffer), _("%s"), desc);
			pango_layout_set_text(layout, buffer, -1);
			pango_cairo_show_layout(cr, layout);
			cairo_move_to(cr,(2 * maxwidth) / (3 * (double) PANGO_SCALE), 0);
			snprintf(buffer, sizeof(buffer), _("%.*f %s"),
					decimals,
					systemweight,
					unit_weight);
			pango_layout_set_text(layout, buffer, -1);
			pango_cairo_show_layout(cr, layout);
			weightsystemcounter++;
			pango_layout_get_extents(layout, &ink_extents, &logical_extents);
			cairo_translate (cr, 0, logical_extents.height / (double) PANGO_SCALE);
		}
	}
	/* Total weight of the system */
	totalweight = get_weight_units(total_weight(dive), &decimals, &unit_weight);
	cairo_move_to (cr, 0, 0);
	snprintf(buffer, sizeof(buffer), _("Total Weight:"));
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
	cairo_move_to(cr,(2 * maxwidth) / (3 * (double) PANGO_SCALE), 0);
	snprintf(buffer, sizeof(buffer), _("%.*f %s\n"),
			decimals,
			totalweight,
			unit_weight);
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
	g_object_unref (layout);

	cairo_restore(cr);
}

/* Print the dive OTUs */
static void print_otus (struct dive *dive, cairo_t *cr, PangoLayout *layout, int maxwidth)
{
	char buffer[40];

	cairo_move_to (cr,(maxwidth*0.05) / ((double) PANGO_SCALE), 0);
	snprintf(buffer, sizeof(buffer), _("OTU"));
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
	cairo_move_to (cr, (2 * maxwidth) / (3 * (double) PANGO_SCALE), 0);
	snprintf(buffer, sizeof(buffer), "%d", dive->otu);
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
}

/* Print the dive maxCNS */
static void print_cns (struct dive *dive, cairo_t *cr, PangoLayout *layout, int maxwidth)
{
	char buffer[40];


	cairo_move_to (cr,(maxwidth*0.05) / ((double) PANGO_SCALE), 0);
	snprintf(buffer, sizeof(buffer), _("Max. CNS"));
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
	cairo_move_to (cr, (2 * maxwidth) / (3 * (double) PANGO_SCALE), 0);
	snprintf(buffer, sizeof(buffer), "%d", dive->maxcns);
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
}

/* Print the SAC */
static void print_SAC (struct dive *dive, cairo_t *cr, 	PangoLayout *layout, int maxwidth)
{
	double sac;
	int decimals;
	const char *unit;
	char buffer[40];

	cairo_move_to (cr,(maxwidth*0.05) / ((double) PANGO_SCALE), 0);
	snprintf(buffer, sizeof(buffer), _("SAC"));
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
	cairo_move_to (cr, maxwidth / (3 * (double) PANGO_SCALE), 0);
	/* Need to change the width, and align because of the size of units string */
	pango_layout_set_width(layout, 3*maxwidth/4);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	sac = get_volume_units(dive->sac, &decimals, &unit);
	snprintf(buffer, sizeof(buffer), "%.*f %s/min",
			decimals,
			sac,
			unit);
	pango_layout_set_text(layout, buffer, -1);
	pango_cairo_show_layout(cr, layout);
	pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
}

/*
 * Show the tanks used in the dive, the mix, and the gas consumed
 * as pressures are shown in the plot. And other data if we used
 * less than four cilynders.
 */
static void show_dive_tanks(struct dive *dive, cairo_t *cr, double w,
	double h, PangoFontDescription *font, double w_scale_factor)
{
	int maxwidth, maxheight, tank_count;
	double line_height, line_width;
	PangoLayout *layout;

	maxwidth = w * PANGO_SCALE;
	maxheight = h * PANGO_SCALE * 0.9;

	/* We need to know how many cylinders we used*/
	for (tank_count = 0; tank_count < MAX_CYLINDERS; tank_count++){
		if (cylinder_nodata(dive->cylinder+tank_count)) {
			break;
		}
	}

	/*Create a frame to separate tank area, note the scaling*/
	cairo_set_line_width(cr, 0.01);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	cairo_rectangle(cr, 0, 0, 2*w, h);
	cairo_move_to(cr, w, 0);
	cairo_line_to(cr, w, h);
	cairo_stroke(cr);

	layout = pango_cairo_create_layout(cr);
	pango_layout_set_height(layout,maxheight);
	pango_layout_set_width(layout, maxwidth/3);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	set_font(layout, font, (FONT_SMALL*(1.5/w_scale_factor)), PANGO_ALIGN_CENTER);
	print_tanks (dive, cr, layout, maxwidth, maxheight, tank_count, 0, font, w_scale_factor);

	/* If there are more than 4 tanks use the full width, else print other data*/
	if (tank_count > 4){
		cairo_save(cr);
		cairo_translate (cr, w, 0);
		layout = pango_cairo_create_layout(cr);
		pango_layout_set_height(layout,maxheight);
		pango_layout_set_width(layout, maxwidth/3);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
		set_font(layout, font, (FONT_SMALL*(1.5/w_scale_factor)), PANGO_ALIGN_CENTER);
		print_tanks (dive, cr, layout, maxwidth, maxheight, tank_count, 4, font, w_scale_factor);
		cairo_restore(cr);
	} else {
		/* Plot a grid for the data */
		line_height = h / 4;
		line_width = w / 2;
		cairo_move_to(cr, 3 * line_width, 0);
		cairo_line_to (cr, 3 * line_width, h);
		cairo_move_to (cr, w, line_height);
		cairo_line_to (cr, 3 * line_width, line_height);
		cairo_move_to (cr, w, 2 * line_height);
		cairo_line_to (cr, 3 * line_width, 2 * line_height);
		cairo_move_to (cr, w, 3 * line_height);
		cairo_line_to (cr, 3 * line_width, 3 * line_height);
		cairo_stroke (cr);
		cairo_save (cr);

		/* and print OTUs, CNS and weight */
		cairo_translate (cr, (3.05 * line_width),0);
		print_weight_data (dive, cr, maxwidth * 0.90/2 , maxheight, font, w_scale_factor);
		cairo_restore (cr);
		cairo_save(cr);

		layout = pango_cairo_create_layout(cr);
		pango_layout_set_height(layout, maxheight);
		pango_layout_set_width(layout, maxwidth);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
		set_font(layout, font, FONT_SMALL*(1.5/w_scale_factor), PANGO_ALIGN_LEFT);

		cairo_translate (cr, w, line_height/4);
		print_SAC (dive, cr, layout, maxwidth * 0.90/2);
		cairo_translate (cr, 0, line_height);
		print_cns (dive, cr, layout, maxwidth * 0.90/2);
		cairo_translate (cr,0, line_height);
		print_otus (dive, cr, layout, maxwidth * 0.90/2);
		cairo_restore (cr);
	}
	g_object_unref (layout);
}

static void show_table_header(cairo_t *cr, double w, double h,
	PangoFontDescription *font)
{
	int i;
	double maxwidth, maxheight, colwidth, curwidth;
	PangoLayout *layout;
	char headers[7][80]= { N_("Dive#"), N_("Date"), N_("Depth"), N_("Duration"), N_("Master"),
			       N_("Buddy"), N_("Location") };

	maxwidth = w * PANGO_SCALE;
	maxheight = h * PANGO_SCALE * 0.9;
	colwidth = maxwidth / 7;

	layout = pango_cairo_create_layout(cr);

	cairo_move_to(cr, 0, 0);
	pango_layout_set_height(layout, maxheight);
	set_font(layout, font, FONT_LARGE, PANGO_ALIGN_LEFT);


	curwidth = 0;
	for (i = 0; i < 7; i++) {
		cairo_move_to(cr, curwidth / PANGO_SCALE, 0);
		if (i == 0 || i == 2 || i == 3){
			// Column 0, 2 and 3 (Dive #, Depth and Time) get 1/2 width
			pango_layout_set_width(layout, colwidth/ (double) 2);
			curwidth = curwidth + (colwidth / 2);
		} else {
			pango_layout_set_width(layout, colwidth);
			curwidth = curwidth + colwidth;
		}
		pango_layout_set_text(layout, _(headers[i]), -1);
		pango_layout_set_justify(layout, 0);
		pango_cairo_show_layout(cr, layout);
	}

	cairo_move_to(cr, 0, 0);
	g_object_unref(layout);
}

static void show_dive_table(struct dive *dive, cairo_t *cr, double w,
	double h, PangoFontDescription *font)
{
	double depth;
	const char *unit;
	int len, decimals;
	double maxwidth, maxheight, colwidth, curwidth;
	PangoLayout *layout;
	struct tm tm;
	char buffer[160], divenr[40];

	maxwidth = w * PANGO_SCALE;
	maxheight = h * PANGO_SCALE * 0.9;

	colwidth = maxwidth / 7;

	layout = pango_cairo_create_layout(cr);

	cairo_move_to(cr, 0, 0);
	pango_layout_set_width(layout, colwidth);
	pango_layout_set_height(layout, maxheight);
	set_font(layout, font, FONT_NORMAL, PANGO_ALIGN_LEFT);
	cairo_move_to(cr, 0, 0);
	curwidth = 0;

	// Col 1: Dive #
	*divenr = 0;
	if (dive->number)
		snprintf(divenr, sizeof(divenr), "#%d", dive->number);
	pango_layout_set_width(layout, colwidth/ (double) 2);
	pango_layout_set_text(layout, divenr, -1);
	pango_layout_set_justify(layout, 0);
	pango_cairo_show_layout(cr, layout);
	curwidth = curwidth + (colwidth / 2);

	// Col 2: Date #
	pango_layout_set_width(layout, colwidth);
	utc_mkdate(dive->when, &tm);
	len = snprintf(buffer, sizeof(buffer),
		/*++GETTEXT 160 chars: weekday, monthname, day, year, hour, min */
		_("%1$s, %2$s %3$d, %4$d   %5$dh%6$02d"),
		weekday(tm.tm_wday),
		monthname(tm.tm_mon),
		tm.tm_mday, tm.tm_year + 1900,
		tm.tm_hour, tm.tm_min
		);
	cairo_move_to(cr, curwidth / PANGO_SCALE, 0);
	pango_layout_set_text(layout, buffer, len);
	pango_layout_set_justify(layout, 0);
	pango_cairo_show_layout(cr, layout);
	curwidth = curwidth + colwidth;

	// Col 3: Depth
	depth = get_depth_units(dive->maxdepth.mm, &decimals, &unit);
	len = snprintf(buffer, sizeof(buffer),
		"%.*f %s", decimals, depth, unit);
	cairo_move_to(cr, curwidth / PANGO_SCALE, 0);
	pango_layout_set_width(layout, colwidth/ (double) 2);
	pango_layout_set_text(layout, buffer, len);
	pango_layout_set_justify(layout, 0);
	pango_cairo_show_layout(cr, layout);
	curwidth = curwidth + (colwidth / 2);

	// Col 4: Time
	len = snprintf(buffer, sizeof(buffer),
		_("%d min"),(dive->duration.seconds + 59) / 60);
	cairo_move_to(cr, curwidth / PANGO_SCALE, 0);
	pango_layout_set_width(layout, colwidth/ (double) 2);
	pango_layout_set_text(layout, buffer, len);
	pango_layout_set_justify(layout, 0);
	pango_cairo_show_layout(cr, layout);
	curwidth = curwidth + (colwidth / 2);

	// Col 5: Master
	pango_layout_set_width(layout, colwidth);
	cairo_move_to(cr, curwidth / PANGO_SCALE, 0);
	pango_layout_set_text(layout, dive->divemaster ? : " ", -1);
	pango_layout_set_justify(layout, 0);
	pango_cairo_show_layout(cr, layout);
	curwidth = curwidth + colwidth;

	// Col 6: Buddy
	cairo_move_to(cr, curwidth / PANGO_SCALE, 0);
	pango_layout_set_text(layout, dive->buddy ? : " ", -1);
	pango_layout_set_justify(layout, 0);
	pango_cairo_show_layout(cr, layout);
	curwidth = curwidth + colwidth;

	// Col 7: Location
	cairo_move_to(cr, curwidth / PANGO_SCALE, 0);
	pango_layout_set_width(layout, maxwidth - curwidth);
	pango_layout_set_text(layout, dive->location ? : " ", -1);
	pango_layout_set_justify(layout, 0);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
}

static void show_dive_profile(struct dive *dive, cairo_t *cr, double w,
	double h)
{
	struct graphics_context gc = {
		.printer = 1,
		.cr = cr,
		.drawing_area = { w/20.0, h/20.0, w, h},
	};
	cairo_save(cr);
	plot(&gc, dive, SC_PRINT);
	cairo_restore(cr);
}

static void print(int divenr, cairo_t *cr, double x, double y, double w,
	double h, PangoFontDescription *font, double w_scale_factor)
{
	struct dive *dive;

	dive = get_dive_for_printing(divenr);
	if (!dive)
		return;
	cairo_save(cr);

	/*Create a frame for each print x,y are provided in draw_page()*/
	cairo_rectangle(cr, x, y, w, h);
	cairo_set_line_width(cr, 0.01);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	cairo_stroke(cr);
	cairo_translate(cr, x, y);

	/* Plus 5% on all sides */
	cairo_translate(cr, w/20, h/20);
	w *= 0.9; h *= 0.9;

	/* We actually want to scale the text and the lines now */
	cairo_scale(cr, 0.5, 0.5);

	/* Dive plot in the upper two thirds - note the scaling */
	show_dive_profile(dive, cr, w*2, h*1.30);

	/* Dive information in the lower third */
	cairo_translate(cr, 0, h*1.30);
	show_dive_header(dive, cr, w*2, h*0.15, font, w_scale_factor);

	cairo_translate(cr, 0, h*0.15);
	show_dive_tanks (dive, cr, w*1, h*0.25, font, w_scale_factor);

	cairo_translate(cr, 0, h*0.25);
	show_dive_notes(dive, cr, w*2, h*0.30, font, w_scale_factor);

	cairo_restore(cr);
}

static void print_table_header(cairo_t *cr, double x, double y,
	double w, double h, PangoFontDescription *font)
{
	cairo_save(cr);
	cairo_translate(cr, x, y);

	/* Plus 5% on all sides */
	cairo_translate(cr, w/20, h/20);
	w *= 0.9; h *= 0.9;

	/* We actually want to scale the text and the lines now */
	cairo_scale(cr, 0.5, 0.5);

	show_table_header(cr, w*2, h*2, font);

	cairo_restore(cr);
}

static void print_table(int divenr, cairo_t *cr, double x, double y,
	double w, double h, PangoFontDescription *font)
{
	struct dive *dive;
	double maxwidth, curwidth;
	int i;

	dive = get_dive_for_printing(divenr);
	if (!dive)
		return;
	cairo_save(cr);

	/*Create a frame for each print x,y are provided in draw_page()*/
	maxwidth = w * 0.90;
	curwidth = w * 0.045;
	cairo_rectangle(cr, curwidth, y, maxwidth, h);
	cairo_set_line_width(cr, 0.01);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	cairo_stroke(cr);
	for (i = 0; i < 6; i++) {
		if (i == 0 || i == 2 || i == 3){
			// Column 0, 2 and 3 (Dive #, Depth and Time) get 1/2 width
			curwidth = curwidth + (maxwidth/7/2);
		} else {
			curwidth = curwidth + (maxwidth/7);
		}
		cairo_move_to(cr, curwidth, y);
		cairo_line_to(cr, curwidth, y + h);
		cairo_set_line_width (cr, 0.01);
		cairo_stroke(cr);
	}

	cairo_translate(cr, x, y);

	/* Plus 5% on all sides */
	cairo_translate(cr, w/20, h/20);
	w *= 0.9; h *= 0.9;

	/* We actually want to scale the text and the lines now */
	cairo_scale(cr, 0.5, 0.5);

	show_dive_table(dive, cr, w*2, h*2, font);

	cairo_restore(cr);
}

/* Positioning factor for height 0,0,1,1,2 ... */
static int pos_h_factor (int z)
{
	int factor = z/2;
	return (factor);
}

/* Positioning factor for width 0,1,0,1 ... */
static int pos_w_factor (int z)
{
	int factor;
	if (z == 0 || z==2 || z==4){
		factor = 0;
	} else {
		factor = 1;
	}
	return (factor);
}

static void cutting_line (cairo_t *cr, double x, double y, double w, double h)
{
	const double dashes[] = {0.05, 0.10};

	cairo_save (cr);
	cairo_translate (cr, x, y);
	cairo_set_line_width (cr,0.01);
	cairo_set_dash (cr, dashes,1,0);
	cairo_move_to (cr, w*1.05,0);
	cairo_line_to (cr, w*1.05, h*1.05);
	cairo_move_to (cr, 0, h*1.05);
	cairo_line_to (cr, w*1.05, h*1.05);
	cairo_stroke (cr);
	cairo_set_dash(cr,0,0,0);
	cairo_restore (cr);
}

static void draw_page(GtkPrintOperation *operation,
			GtkPrintContext *context,
			gint page_nr,
			Print_params *print_params)
{
	int nr, i, dives_per_print;
	cairo_t *cr;
	double w, h;
	PangoFontDescription *font;

	cr = gtk_print_context_get_cairo_context(context);
	font = pango_font_description_from_string("Sans");
	/* scale the width and height by defined factors */
	w = gtk_print_context_get_width(context)/print_params->w_scale;
	h = gtk_print_context_get_height(context)/print_params->h_scale;
	/* set the n� of dives to show */
	dives_per_print = print_params->dives;
	nr = page_nr * dives_per_print;
	/* choose if rotate the layouts 90� in rad */
	if (print_params->rotation == 1){
		cairo_translate (cr, 0, h*print_params->h_scale);
		cairo_rotate (cr, -1.57);
	}

	/*
	 * Calls print () as many times as defined.
	 * In two prints option leaves room between prints and plot
	 * a dashed line for cutting.
	 */
	for (i = 0; i < dives_per_print; i++)
	{
		double pos_w = pos_w_factor(i);
		double pos_h = pos_h_factor(i);

		if (i == 1 && dives_per_print == 2)
			pos_w = 1.1;
		if (dives_per_print == 2)
			cutting_line (cr, w*pos_w, h*pos_h, w, h);
		print(nr+i, cr, w*pos_w, h*pos_h, w, h, font, print_params->w_scale);
	}
	pango_font_description_free(font);
}

static void draw_table(GtkPrintOperation *operation,
			GtkPrintContext *context,
			gint page_nr,
			gpointer user_data)
{
	int i, nr;
	int n_dive_per_page = 25;
	cairo_t *cr;
	double w, h;
	PangoFontDescription *font;

	cr = gtk_print_context_get_cairo_context(context);
	font = pango_font_description_from_string("Sans");

	w = gtk_print_context_get_width(context);
	h = gtk_print_context_get_height(context)/(n_dive_per_page+1);

	nr = page_nr*n_dive_per_page;
	print_table_header(cr, 0, 0+h, w, h, font);
	for (i = 0; i < n_dive_per_page; i++) {
		print_table(nr+i, cr, 0,   h*1.5+h*i, w, h, font);
	}

	pango_font_description_free(font);
}

static int nr_selected_dives(void)
{
	int i, dives;
	struct dive *dive;

	dives = 0;
	for_each_dive(i, dive)
		dives += dive->selected;
	return dives;
}

static void begin_print(GtkPrintOperation *operation, gpointer user_data)
{
	int pages, dives;
	int dives_per_page;

	dives = nr_selected_dives();
	if (!print_options.print_selected)
		dives = dive_table.nr;

	if (print_options.type == PRETTY) {
		dives_per_page = 6;
	} else if (print_options.type == TWOPERPAGE) {
		dives_per_page = 2;
	} else {
		dives_per_page = 25;
	}
	pages = (dives + dives_per_page - 1) / dives_per_page;
	gtk_print_operation_set_n_pages(operation, pages);
}

#define OPTIONCALLBACK(name, type, value) \
static void name(GtkWidget *w, gpointer data) \
{\
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w))) \
		print_options.type = value; \
}

OPTIONCALLBACK(set_pretty, type, PRETTY)
OPTIONCALLBACK(set_table, type, TABLE)
OPTIONCALLBACK(set_twoperpage, type, TWOPERPAGE)

#define OPTIONSELECTEDCALLBACK(name, option) \
static void name(GtkWidget *w, gpointer data) \
{ \
	option = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)); \
}

OPTIONSELECTEDCALLBACK(print_selection_toggle, print_options.print_selected)


static GtkWidget *print_dialog(GtkPrintOperation *operation, gpointer user_data)
{
	GtkWidget *vbox, *radio1, *radio2, *radio3, *frame, *box;
	int dives;
	gtk_print_operation_set_custom_tab_label(operation, _("Dive details"));

	vbox = gtk_vbox_new(TRUE, 5);

	frame = gtk_frame_new(_("Print type"));
	gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 1);

	box = gtk_hbox_new(FALSE, 2);
	gtk_container_add(GTK_CONTAINER(frame), box);

	radio1 = gtk_radio_button_new_with_label (NULL, _("6 dives per page"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio1),
		print_options.type == PRETTY);
	radio2 = gtk_radio_button_new_with_label_from_widget (
		GTK_RADIO_BUTTON (radio1), _("2 dives per page"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio2),
		print_options.type == TWOPERPAGE);
	radio3 = gtk_radio_button_new_with_label_from_widget (
		GTK_RADIO_BUTTON (radio1), _("Table print"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio3),
		print_options.type == TABLE);
	gtk_box_pack_start (GTK_BOX (box), radio1, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (box), radio2, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (box), radio3, TRUE, TRUE, 0);

	g_signal_connect(radio1, "toggled", G_CALLBACK(set_pretty), NULL);
	g_signal_connect(radio2, "toggled", G_CALLBACK(set_twoperpage), NULL);
	g_signal_connect(radio3, "toggled", G_CALLBACK(set_table), NULL);

	dives = nr_selected_dives();
	print_options.print_selected = dives >= 1;
	if (print_options.print_selected) {
		frame = gtk_frame_new(_("Print selection"));
		gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 1);
		box = gtk_hbox_new(FALSE, 1);
		gtk_container_add(GTK_CONTAINER(frame), box);
		GtkWidget *button;
		button = gtk_check_button_new_with_label(_("Print only selected dives"));
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button),
			print_options.print_selected);
		gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 2);
		g_signal_connect(G_OBJECT(button), "toggled",
			G_CALLBACK(print_selection_toggle), NULL);
	}

	gtk_widget_show_all(vbox);
	return vbox;
}

static void print_dialog_apply(GtkPrintOperation *operation, GtkWidget *widget, gpointer user_data)
{
	Print_params *print_params = g_slice_new(Print_params);

	if (print_options.type == PRETTY) {
		print_params->dives = 6;
		print_params->h_scale = 3;
		print_params->w_scale = 2;
		print_params->rotation = 0;
		g_signal_connect(operation, "draw_page", G_CALLBACK(draw_page), print_params);
	} else {
		if (print_options.type == TWOPERPAGE) {
			print_params->dives = 2;
			print_params->h_scale = 1.6;
			print_params->w_scale = 1.8;
			print_params->rotation = 1;
			g_signal_connect(operation, "draw_page", G_CALLBACK(draw_page), print_params);
		} else {
			g_signal_connect(operation, "draw_page", G_CALLBACK(draw_table), NULL);
		}
	}
}

static GtkPrintSettings *settings = NULL;

void do_print(void)
{
	GtkPrintOperation *print;
	GtkPrintOperationResult res;

	repaint_dive();
	print = gtk_print_operation_new();
	gtk_print_operation_set_unit(print, GTK_UNIT_INCH);
	if (settings != NULL)
		gtk_print_operation_set_print_settings(print, settings);
	g_signal_connect(print, "create-custom-widget", G_CALLBACK(print_dialog), NULL);
	g_signal_connect(print, "custom-widget-apply", G_CALLBACK(print_dialog_apply), NULL);
	g_signal_connect(print, "begin_print", G_CALLBACK(begin_print), NULL);
	res = gtk_print_operation_run(print, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
					 GTK_WINDOW(main_window), NULL);
	if (res == GTK_PRINT_OPERATION_RESULT_APPLY) {
		if (settings != NULL)
			g_object_unref(settings);
		settings = g_object_ref(gtk_print_operation_get_print_settings(print));
	}
	g_object_unref(print);
}
