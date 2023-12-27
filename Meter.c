/*
htop - Meter.c
(C) 2004-2011 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "Meter.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "CRT.h"
#include "Macros.h"
#include "Object.h"
#include "ProvideCurses.h"
#include "RichString.h"
#include "Row.h"
#include "Settings.h"
#include "XUtils.h"


#ifndef UINT16_WIDTH /* Defined in the C23 standard */
#define UINT16_WIDTH 16
#endif

#define DEFAULT_GRAPH_HEIGHT 4 /* Unit: rows (lines) */
#define MAX_GRAPH_HEIGHT 8191 /* == (int)(UINT16_MAX / 8) */

const MeterClass Meter_class = {
   .super = {
      .extends = Class(Object)
   }
};

Meter* Meter_new(const Machine* host, unsigned int param, const MeterClass* type) {
   Meter* this = xCalloc(1, sizeof(Meter));
   Object_setClass(this, type);
   this->h = 1;
   this->param = param;
   this->host = host;
   this->curItems = type->maxItems;
   this->curAttributes = NULL;
   this->values = type->maxItems ? xCalloc(type->maxItems, sizeof(double)) : NULL;
   this->total = type->total;
   this->caption = xStrdup(type->caption);
   if (Meter_initFn(this)) {
      Meter_init(this);
   }
   Meter_setMode(this, type->defaultMode);
   return this;
}

/* Converts 'value' in kibibytes into a human readable string.
   Example output strings: "0K", "1023K", "98.7M" and "1.23G" */
int Meter_humanUnit(char* buffer, double value, size_t size) {
   size_t i = 0;

   assert(value >= 0.0);
   while (value >= ONE_K) {
      if (i >= ARRAYSIZE(unitPrefixes) - 1) {
         if (value > 9999.0) {
            return xSnprintf(buffer, size, "inf");
         }
         break;
      }

      value /= ONE_K;
      ++i;
   }

   int precision = 0;

   if (i > 0) {
      // Fraction digits for mebibytes and above
      precision = value <= 99.9 ? (value <= 9.99 ? 2 : 1) : 0;

      // Round up if 'value' is in range (99.9, 100) or (9.99, 10)
      if (precision < 2) {
         double limit = precision == 1 ? 10.0 : 100.0;
         if (value < limit) {
            value = limit;
         }
      }
   }

   return xSnprintf(buffer, size, "%.*f%c", precision, value, unitPrefixes[i]);
}

void Meter_delete(Object* cast) {
   if (!cast)
      return;

   Meter* this = (Meter*) cast;
   if (Meter_doneFn(this)) {
      Meter_done(this);
   }
   free(this->drawData.buffer);
   free(this->caption);
   free(this->values);
   free(this);
}

void Meter_setCaption(Meter* this, const char* caption) {
   free_and_xStrdup(&this->caption, caption);
}

static inline void Meter_displayBuffer(const Meter* this, RichString* out) {
   if (Object_displayFn(this)) {
      Object_display(this, out);
   } else {
      RichString_writeWide(out, CRT_colors[Meter_attributes(this)[0]], this->txtBuffer);
   }
}

void Meter_setMode(Meter* this, int modeIndex) {
   if (modeIndex > 0 && modeIndex == this->mode) {
      return;
   }

   if (!modeIndex) {
      modeIndex = 1;
   }

   assert(modeIndex < LAST_METERMODE);
   if (Meter_defaultMode(this) == CUSTOM_METERMODE) {
      this->draw = Meter_drawFn(this);
      if (Meter_updateModeFn(this)) {
         Meter_updateMode(this, modeIndex);
      }
   } else {
      assert(modeIndex >= 1);
      free(this->drawData.buffer);
      this->drawData.buffer = NULL;
      this->drawData.nValues = 0;

      const MeterMode* mode = Meter_modes[modeIndex];
      this->draw = mode->draw;
      this->h = mode->h;
   }
   this->mode = modeIndex;
}

ListItem* Meter_toListItem(const Meter* this, bool moving) {
   char mode[20];
   if (this->mode) {
      xSnprintf(mode, sizeof(mode), " [%s]", Meter_modes[this->mode]->uiName);
   } else {
      mode[0] = '\0';
   }
   char name[32];
   if (Meter_getUiNameFn(this))
      Meter_getUiName(this, name, sizeof(name));
   else
      xSnprintf(name, sizeof(name), "%s", Meter_uiName(this));
   char buffer[50];
   xSnprintf(buffer, sizeof(buffer), "%s%s", name, mode);
   ListItem* li = ListItem_new(buffer, 0);
   li->moving = moving;
   return li;
}

static double Meter_computeSum(const Meter* this) {
   double sum = sumPositiveValues(this->values, this->curItems);
   // Prevent rounding to infinity in IEEE 754
   if (sum > DBL_MAX)
      return DBL_MAX;

   return sum;
}

/* ---------- TextMeterMode ---------- */

static void TextMeterMode_draw(Meter* this, int x, int y, int w) {
   const char* caption = Meter_getCaption(this);
   attrset(CRT_colors[METER_TEXT]);
   mvaddnstr(y, x, caption, w);
   attrset(CRT_colors[RESET_COLOR]);

   int captionLen = strlen(caption);
   x += captionLen;
   w -= captionLen;
   if (w <= 0)
      return;

   RichString_begin(out);
   Meter_displayBuffer(this, &out);
   RichString_printoffnVal(out, y, x, 0, w);
   RichString_delete(&out);
}

/* ---------- BarMeterMode ---------- */

static const char BarMeterMode_characters[] = "|#*@$%&.";

static void BarMeterMode_draw(Meter* this, int x, int y, int w) {
   const char* caption = Meter_getCaption(this);
   attrset(CRT_colors[METER_TEXT]);
   int captionLen = 3;
   mvaddnstr(y, x, caption, captionLen);
   x += captionLen;
   w -= captionLen;
   attrset(CRT_colors[BAR_BORDER]);
   mvaddch(y, x, '[');
   w--;
   mvaddch(y, x + MAXIMUM(w, 0), ']');
   w--;
   attrset(CRT_colors[RESET_COLOR]);

   x++;

   if (w < 1)
      return;

   // The text in the bar is right aligned;
   // Pad with maximal spaces and then calculate needed starting position offset
   RichString_begin(bar);
   RichString_appendChr(&bar, 0, ' ', w);
   RichString_appendWide(&bar, 0, this->txtBuffer);
   int startPos = RichString_sizeVal(bar) - w;
   if (startPos > w) {
      // Text is too large for bar
      // Truncate meter text at a space character
      for (int pos = 2 * w; pos > w; pos--) {
         if (RichString_getCharVal(bar, pos) == ' ') {
            while (pos > w && RichString_getCharVal(bar, pos - 1) == ' ')
               pos--;
            startPos = pos - w;
            break;
         }
      }

      // If still too large, print the start not the end
      startPos = MINIMUM(startPos, w);
   }
   assert(startPos >= 0);
   assert(startPos <= w);
   assert(startPos + w <= RichString_sizeVal(bar));

   int blockSizes[10];

   // First draw in the bar[] buffer...
   int offset = 0;
   if (!Meter_isPercentChart(this) && this->curItems > 0) {
      double sum = Meter_computeSum(this);
      if (this->total < sum)
         this->total = sum;
   }
   for (uint8_t i = 0; i < this->curItems; i++) {
      double value = this->values[i];
      if (isPositive(value) && this->total > 0.0) {
         value = MINIMUM(value, this->total);
         blockSizes[i] = ceil((value / this->total) * w);
      } else {
         blockSizes[i] = 0;
      }
      int nextOffset = offset + blockSizes[i];
      // (Control against invalid values)
      nextOffset = CLAMP(nextOffset, 0, w);
      for (int j = offset; j < nextOffset; j++)
         if (RichString_getCharVal(bar, startPos + j) == ' ') {
            if (CRT_colorScheme == COLORSCHEME_MONOCHROME) {
               assert(i < strlen(BarMeterMode_characters));
               RichString_setChar(&bar, startPos + j, BarMeterMode_characters[i]);
            } else {
               RichString_setChar(&bar, startPos + j, '|');
            }
         }
      offset = nextOffset;
   }

   // ...then print the buffer.
   offset = 0;
   for (uint8_t i = 0; i < this->curItems; i++) {
      int attr = this->curAttributes ? this->curAttributes[i] : Meter_attributes(this)[i];
      RichString_setAttrn(&bar, CRT_colors[attr], startPos + offset, blockSizes[i]);
      RichString_printoffnVal(bar, y, x + offset, startPos + offset, MINIMUM(blockSizes[i], w - offset));
      offset += blockSizes[i];
      offset = CLAMP(offset, 0, w);
   }
   if (offset < w) {
      RichString_setAttrn(&bar, CRT_colors[BAR_SHADOW], startPos + offset, w - offset);
      RichString_printoffnVal(bar, y, x + offset, startPos + offset, w - offset);
   }

   RichString_delete(&bar);

   move(y, x + w + 1);
   attrset(CRT_colors[RESET_COLOR]);
}

/* ---------- GraphMeterMode ---------- */

#if 0 /* Used in old graph meter drawing code; to be removed */
#ifdef HAVE_LIBNCURSESW

#define PIXPERROW_UTF8 4
static const char* const GraphMeterMode_dotsUtf8[] = {
   /*00*/" ", /*01*/"⢀", /*02*/"⢠", /*03*/"⢰", /*04*/ "⢸",
   /*10*/"⡀", /*11*/"⣀", /*12*/"⣠", /*13*/"⣰", /*14*/ "⣸",
   /*20*/"⡄", /*21*/"⣄", /*22*/"⣤", /*23*/"⣴", /*24*/ "⣼",
   /*30*/"⡆", /*31*/"⣆", /*32*/"⣦", /*33*/"⣶", /*34*/ "⣾",
   /*40*/"⡇", /*41*/"⣇", /*42*/"⣧", /*43*/"⣷", /*44*/ "⣿"
};

#endif

#define PIXPERROW_ASCII 2
static const char* const GraphMeterMode_dotsAscii[] = {
   /*00*/" ", /*01*/".", /*02*/":",
   /*10*/".", /*11*/".", /*12*/":",
   /*20*/":", /*21*/":", /*22*/":"
};
#endif

static void GraphMeterMode_reallocateGraphBuffer(Meter* this, const GraphDrawContext* context, size_t nValues) {
   GraphData* data = &this->drawData;

   size_t nCellsPerValue = context->nCellsPerValue;
   size_t valueSize = nCellsPerValue * sizeof(*data->buffer);

   if (!valueSize)
      goto bufferInitialized;

   data->buffer = xReallocArray(data->buffer, nValues, valueSize);

   // Move existing records ("values") to correct position
   assert(nValues >= data->nValues);
   size_t moveOffset = (nValues - data->nValues) * nCellsPerValue;
   memmove(data->buffer + moveOffset, data->buffer, data->nValues * valueSize);

   // Fill new spaces with blank records
   memset(data->buffer, 0, moveOffset * sizeof(*data->buffer));
   if (context->maxItems > 1) {
      for (size_t i = 0; i < moveOffset; i++) {
         if (context->isPercentChart || i % nCellsPerValue > 0) {
            data->buffer[i].c.itemIndex = UINT8_MAX;
         }
      }
   }

bufferInitialized:
   data->nValues = nValues;
}

static unsigned int GraphMeterMode_valueCellIndex(unsigned int graphHeight, bool isPercentChart, int deltaExp, unsigned int y, unsigned int* scaleFactor, unsigned int* increment) {
   if (scaleFactor)
      *scaleFactor = 1;

   assert(deltaExp >= 0);
   assert(deltaExp < UINT16_WIDTH);
   unsigned int yTop = (graphHeight - 1) >> deltaExp;
   if (isPercentChart) {
      assert(deltaExp == 0);
      if (increment)
         *increment = 1;

      if (y > yTop)
         return (unsigned int)-1;

      return y;
   }
   // A record may be rendered in different scales depending on the largest
   // "scaleExp" value of a record set. The colors are precomputed for
   // different scales of the same record. It takes (2 * graphHeight - 1) cells
   // of space to store all the color information.
   //
   // An example for graphHeight = 6:
   //
   //    scale  1*n  2*n  4*n  8*n 16*n | n = value sum of all items
   // --------------------------------- |     rounded up to a power of
   // deltaExp    0    1    2    3    4 |     two. The exponent of n is
   // --------------------------------- |     stored in index [0].
   //    array [11]    X    X    X    X | X = empty cell
   //  indices  [9]    X    X    X    X | Cells whose array indices
   //           [7]    X    X    X    X | are >= (2 * graphHeight) are
   //           [5] [10]    X    X    X | computed from cells of a
   //           [3]  [6] (12)    X    X | lower scale and not stored in
   //           [1]  [2]  [4]  [8] (16) | the array.
   if (increment)
      *increment = 2U << deltaExp;

   if (y > yTop)
      return (unsigned int)-1;

   // "b" is the "base" offset or the upper bits of offset
   unsigned int b = (y * 2) << deltaExp;
   unsigned int offset = 1U << deltaExp;
   if (y == yTop) {
      assert(((2 * graphHeight - 1) & b) == b);
      unsigned int offsetTop = powerOf2Floor(2 * graphHeight - 1 - b);
      if (scaleFactor && offsetTop) {
         *scaleFactor = offset / offsetTop;
      }
      return b + offsetTop;
   }
   return b + offset;
}

static uint8_t GraphMeterMode_findTopCellItem(const Meter* this, double scaledTotal, unsigned int topCell) {
   unsigned int graphHeight = (unsigned int)this->h;
   assert(topCell < graphHeight);

   double valueSum = 0.0;
   double prevTopPoint = (double)(int)topCell;
   double maxArea = 0.0;
   uint8_t topCellItem = this->curItems - 1;
   for (uint8_t i = 0; i < this->curItems && valueSum < DBL_MAX; i++) {
      if (!isPositive(this->values[i]))
         continue;

      valueSum += this->values[i];
      if (valueSum > DBL_MAX)
         valueSum = DBL_MAX;

      double topPoint = (valueSum / scaledTotal) * (double)(int)graphHeight;
      if (topPoint > prevTopPoint) {
         // Find the item that occupies the largest area of the top cell.
         // Favor item with higher index in case of a tie.
         if (topPoint - prevTopPoint >= maxArea) {
            topCellItem = i;
            maxArea = topPoint - prevTopPoint;
         }
         prevTopPoint = topPoint;
      }
   }
   return topCellItem;
}

static void GraphMeterMode_computeColors(Meter* this, const GraphDrawContext* context, GraphColorCell* valueStart, int deltaExp, double scaledTotal, int numDots) {
   unsigned int graphHeight = (unsigned int)this->h;
   bool isPercentChart = context->isPercentChart;

   assert(deltaExp >= 0);
   assert(numDots > 0);
   assert((unsigned int)numDots <= graphHeight * 8);

   // If there is a "top cell" which will not be completely filled, determine
   // its color first.
   unsigned int topCell = ((unsigned int)numDots - 1) / 8;
   const uint8_t dotAlignment = 2;
   unsigned int blanksAtTopCell = ((topCell + 1) * 8 - (unsigned int)numDots) / dotAlignment * dotAlignment;

   bool hasPartialTopCell = false;
   if (blanksAtTopCell > 0) {
      hasPartialTopCell = true;
   } else if (!isPercentChart && topCell == ((graphHeight - 1) >> deltaExp) && topCell % 2 == 0) {
      // This "top cell" is rendered as full in one scale, but partial in the
      // next scale. (Only happens when graphHeight is not a power of two.)
      hasPartialTopCell = true;
   }

   double topCellArea = 0.0;
   uint8_t topCellItem = this->curItems - 1;
   if (hasPartialTopCell) {
      topCellArea = (8 - (int)blanksAtTopCell) / 8.0;
      topCellItem = GraphMeterMode_findTopCellItem(this, scaledTotal, topCell);
   }
   topCell += 1; // This index points to a cell that would be blank.

   // NOTE: The algorithm below needs a rewrite. It's messy for now and don't expect the result would be accurate.
   // BEGINNING OF THE PART THAT NEEDS REWRITING

   // Compute colors of the rest of the cells, using the largest remainder
   // method (a.k.a. Hamilton's method).
   // The Hare quota is (scaledTotal / graphHeight).
   int paintedHigh = (int)topCell + (int)topCellItem + 1;
   int paintedLow = 0;
   double threshold = 0.5;
   double thresholdHigh = 1.0;
   double thresholdLow = 0.0;
   // Tiebreak 1: Favor items with less number of cells. (Top cell is not
   // included in the count.)
   int cellLimit = (int)topCell;
   int cellLimitHigh = (int)topCell;
   int cellLimitLow = 0.0;
   // Tiebreak 2: Favor items whose indices are lower.
   uint8_t tiedItemLimit = topCellItem;
   while (true) {
      double sum = 0.0;
      double bottom = 0.0;
      int cellsPainted = 0;
      double nextThresholdHigh = 0.0;
      double nextThresholdLow = 1.0;
      int nextCellLimitHigh = 0;
      int nextCellLimitLow = (int)topCell;
      uint8_t numTiedItems = 0;
      for (uint8_t i = 0; i <= topCellItem && sum < DBL_MAX; i++) {
         if (!isPositive(this->values[i]))
            continue;

         sum += this->values[i];
         if (sum > DBL_MAX)
            sum = DBL_MAX;

         double top = (sum / scaledTotal) * graphHeight;
         double area = top - bottom;
         double rem = area;
         if (i == topCellItem) {
            rem -= topCellArea;
            if (!(rem >= 0.0))
               rem = 0.0;
         }
         int numCells = (int)rem;
         rem -= numCells;

         // Whether the item will receive an extra cell or be truncated
         if (rem >= threshold) {
            if (rem > threshold) {
               if (rem < nextThresholdLow)
                  nextThresholdLow = rem;
               numCells++;
            } else if (numCells <= cellLimit) {
               if (numCells < cellLimit) {
                  if (numCells > nextCellLimitHigh)
                     nextCellLimitHigh = numCells;
                  numCells++;
               } else {
                  numTiedItems++;
                  if (numTiedItems <= tiedItemLimit) {
                     numCells++;
                  } else {
                     rem = 0.0;
                  }
               }
            } else {
               if (numCells < nextCellLimitLow)
                  nextCellLimitLow = numCells;
               rem = 0.0;
            }
         } else {
            if (rem > nextThresholdHigh)
               nextThresholdHigh = rem;
            rem = 0.0;
         }

         // Paint cells to the buffer
         uint8_t blanksAtEnd = 0;
         if (i == topCellItem && topCellArea > 0.0) {
            numCells++;
            if (area < topCellArea) {
               rem = MAXIMUM(area, 0.25);
               blanksAtEnd = (uint8_t)blanksAtTopCell;
            }
         } else if (cellsPainted + numCells >= (int)topCell) {
            blanksAtEnd = 0;
         } else if (cellsPainted <= 0 || bottom <= cellsPainted) {
            blanksAtEnd = ((uint8_t)((1.0 - rem) * 8.0) % 8);
         } else if (cellsPainted + numCells > top) {
            assert(cellsPainted + numCells - top < 1.0);
            blanksAtEnd = (uint8_t)((cellsPainted + numCells - top) * 8.0);
         }

         unsigned int blanksAtStart = 0;
         if (cellsPainted > 0) {
            blanksAtStart = ((uint8_t)((1.0 - rem) * 8.0) % 8 - blanksAtEnd);
         }

         while (numCells > 0 && cellsPainted < (int)topCell) {
            unsigned int offset = (unsigned int)cellsPainted;
            if (!isPercentChart) {
               offset = (offset * 2 + 1) << deltaExp;
            }

            valueStart[offset].c.itemIndex = (uint8_t)i;
            valueStart[offset].c.details = 0xFF;

            if (blanksAtStart > 0) {
               assert(blanksAtStart < 8);
               valueStart[offset].c.details >>= blanksAtStart;
               blanksAtStart = 0;
            }

            if (cellsPainted == (int)topCell - 1) {
               assert(blanksAtTopCell < 8);
               valueStart[offset].c.details &= 0xFF << blanksAtTopCell;
            } else if (numCells == 1) {
               assert(blanksAtEnd < 8);
               valueStart[offset].c.details &= 0xFF << blanksAtEnd;
            }

            numCells--;
            cellsPainted++;
         }
         cellsPainted += numCells;

         bottom = top;
      }

      if (cellsPainted == (int)topCell)
         break;

      // Set new bounds and threshold
      if (cellsPainted > (int)topCell) {
         paintedHigh = cellsPainted;
         if (thresholdLow >= thresholdHigh) {
            if (cellLimitLow >= cellLimitHigh) {
               assert(tiedItemLimit >= topCellItem);
               tiedItemLimit = numTiedItems - (uint8_t)(cellsPainted - (int)topCell);
            } else {
               assert(cellLimitHigh > cellLimit);
               cellLimitHigh = cellLimit;
            }
         } else {
            assert(thresholdLow < threshold);
            thresholdLow = threshold;
         }
      } else {
         paintedLow = cellsPainted + 1;
         if (thresholdLow >= thresholdHigh) {
            assert(cellLimitLow < cellLimitHigh);
            assert(cellLimitLow < nextCellLimitLow);
            cellLimitLow = nextCellLimitLow;
            nextCellLimitHigh = cellLimitHigh;
         } else {
            assert(cellLimit >= (int)topCell);
            assert(thresholdHigh > nextThresholdHigh);
            thresholdHigh = nextThresholdHigh;
            nextThresholdLow = thresholdLow;
         }
      }
      threshold = thresholdHigh;
      if (thresholdLow >= thresholdHigh) {
         cellLimit = cellLimitLow;
         if (cellLimitLow < cellLimitHigh && paintedHigh > paintedLow) {
            cellLimit += ((cellLimitHigh - cellLimitLow) *
                         ((int)topCell - paintedLow) /
                         (paintedHigh - paintedLow));
            if (cellLimit > nextCellLimitHigh)
               cellLimit = nextCellLimitHigh;
         }
      } else {
         if (paintedHigh > paintedLow) {
            threshold -= ((thresholdHigh - thresholdLow) *
                         ((int)topCell - paintedLow) /
                         (paintedHigh - paintedLow));
            if (threshold < nextThresholdLow)
               threshold = nextThresholdLow;
         }
      }
      assert(threshold <= thresholdHigh);
   }
   // END OF THE PART THAT NEEDS REWRITING
}

static void GraphMeterMode_recordNewValue(Meter* this, const GraphDrawContext* context) {
   uint8_t maxItems = context->maxItems;
   bool isPercentChart = context->isPercentChart;
   size_t nCellsPerValue = context->nCellsPerValue;
   if (!nCellsPerValue)
      return;

   GraphData* data = &this->drawData;
   size_t nValues = data->nValues;
   unsigned int graphHeight = (unsigned int)this->h;

   // Move previous records
   size_t valueSize = nCellsPerValue * sizeof(*data->buffer);
   memmove(&data->buffer[0], &data->buffer[1 * nCellsPerValue], (nValues - 1) * valueSize);

   GraphColorCell* valueStart = &data->buffer[(nValues - 1) * nCellsPerValue];

   // Compute "sum" and "total"
   double sum = Meter_computeSum(this);
   assert(sum >= 0.0);
   assert(sum <= DBL_MAX);
   double total;
   int scaleExp = 0;
   if (isPercentChart) {
      total = MAXIMUM(this->total, sum);
   } else {
      (void) frexp(sum, &scaleExp);
      if (scaleExp < 0) {
         scaleExp = 0;
      }
      // In IEEE 754 binary64 (DBL_MAX_EXP == 1024, DBL_MAX_10_EXP == 308),
      // "scaleExp" never overflows.
      assert(DBL_MAX_10_EXP < 9864);
      assert(scaleExp <= INT16_MAX);
      valueStart[0].scaleExp = (int16_t)scaleExp;
      total = ldexp(1.0, scaleExp);
   }
   if (total > DBL_MAX)
      total = DBL_MAX;

   assert(graphHeight <= UINT16_MAX / 8);
   double maxDots = (double)(int32_t)(graphHeight * 8);
   int numDots = (int) ceil((sum / total) * maxDots);
   assert(numDots >= 0);
   if (sum > 0.0 && numDots <= 0) {
      numDots = 1; // Division of (sum / total) underflows
   }

   if (maxItems == 1) {
      assert(numDots <= UINT16_MAX);
      valueStart[isPercentChart ? 0 : 1].numDots = (uint16_t)numDots;
      return;
   }

   // Clear cells
   unsigned int i = ((unsigned int)numDots + 8 - 1) / 8; // Round up
   i = GraphMeterMode_valueCellIndex(graphHeight, isPercentChart, 0, i, NULL, NULL);
   for (; i < nCellsPerValue; i++) {
      valueStart[i].c.itemIndex = UINT8_MAX;
      valueStart[i].c.details = 0x00;
   }

   if (sum <= 0.0)
      return;

   int deltaExp = 0;
   double scaledTotal = total;
   while (true) {
      numDots = (int) ceil((sum / scaledTotal) * maxDots);
      if (numDots <= 0) {
         numDots = 1; // Division of (sum / scaledTotal) underflows
      }

      GraphMeterMode_computeColors(this, context, valueStart, deltaExp, scaledTotal, numDots);

      if (isPercentChart || !(scaledTotal < DBL_MAX) || (1U << deltaExp) >= graphHeight) {
         break;
      }

      deltaExp++;
      scaledTotal *= 2.0;
      if (scaledTotal > DBL_MAX) {
         scaledTotal = DBL_MAX;
      }
   }
}

static void GraphMeterMode_printScale(int exponent) {
   if (exponent < 10) {
      // "1" to "512"; the (exponent < 0) case is not implemented.
      assert(exponent >= 0);
      printw("%3u", 1U << exponent);
   } else if (exponent > (int)ARRAYSIZE(unitPrefixes) * 10 + 6) {
      addstr("inf");
   } else if (exponent % 10 < 7) {
      // "1K" to "64K", "1M" to "64M", "1G" to "64G", etc.
      printw("%2u%c", 1U << (exponent % 10), unitPrefixes[exponent / 10 - 1]);
   } else {
      // "M/8" (=128K), "M/4" (=256K), "M/2" (=512K), "G/8" (=128M), etc.
      printw("%c/%u", unitPrefixes[exponent / 10], 1U << (10 - exponent % 10));
   }
}

static uint8_t GraphMeterMode_scaleCellDetails(uint8_t details, unsigned int scaleFactor) {
   // Only the "top cell" of a record may need scaling like this; the cell does
   // not use the special meaning of bit 4.
   // This algorithm assumes the "details" be printed in braille characters.
   assert(scaleFactor > 0);
   if (scaleFactor < 2) {
      return details;
   }
   if (scaleFactor < 4 && (details & 0x0F) != 0x00) {
      // Display the cell in half height (bits 0 to 3 are zero).
      // Bits 4 and 5 are set simultaneously to avoid a jaggy visual.
      uint8_t newDetails = 0x30;
      // Bit 6
      if (popCount8(details) > 4)
         newDetails |= 0x40;
      // Bit 7 (equivalent to (details >= 0x80 || popCount8(details) > 6))
      if (details >= 0x7F)
         newDetails |= 0x80;
      return newDetails;
   }
   if (details != 0x00) {
      // Display the cell in a quarter height (bits 0 to 5 are zero).
      // Bits 6 and 7 are set simultaneously.
      return 0xC0;
   }
   return 0x00;
}

static int GraphMeterMode_lookupCell(const Meter* this, const GraphDrawContext* context, int scaleExp, size_t valueIndex, unsigned int y, uint8_t* details) {
   unsigned int graphHeight = (unsigned int)this->h;
   const GraphData* data = &this->drawData;

   uint8_t maxItems = context->maxItems;
   bool isPercentChart = context->isPercentChart;
   size_t nCellsPerValue = context->nCellsPerValue;

   // Reverse the coordinate
   assert(y < graphHeight);
   y = graphHeight - 1 - y;

   uint8_t itemIndex = UINT8_MAX;
   *details = 0x00; // Empty the cell

   if (maxItems < 1)
      goto cellIsEmpty;

   assert(valueIndex < data->nValues);
   const GraphColorCell* valueStart = &data->buffer[valueIndex * nCellsPerValue];

   int deltaExp = isPercentChart ? 0 : scaleExp - valueStart[0].scaleExp;
   assert(deltaExp >= 0);

   if (maxItems == 1) {
      unsigned int numDots = valueStart[isPercentChart ? 0 : 1].numDots;

      if (numDots < 1)
         goto cellIsEmpty;

      // Scale according to exponent difference. Round up.
      numDots = deltaExp < UINT16_WIDTH ? ((numDots - 1) >> deltaExp) + 1 : 1;

      if (y * 8 >= numDots)
         goto cellIsEmpty;

      itemIndex = 0;
      *details = 0xFF;
      if ((y + 1) * 8 > numDots) {
         const uint8_t dotAlignment = 2;
         unsigned int blanksAtTopCell = ((y + 1) * 8 - numDots) / dotAlignment * dotAlignment;
         *details <<= blanksAtTopCell;
      }
   } else {
      int deltaExpArg = deltaExp >= UINT16_WIDTH ? UINT16_WIDTH - 1 : deltaExp;

      unsigned int scaleFactor;
      unsigned int i = GraphMeterMode_valueCellIndex(graphHeight, isPercentChart, deltaExpArg, y, &scaleFactor, NULL);
      if (i == (unsigned int)-1)
         goto cellIsEmpty;

      if (deltaExp >= UINT16_WIDTH) {
         // Any "scaleFactor" value greater than 8 behaves the same as 8 for the
         // "scaleCellDetails" function.
         scaleFactor = 8;
      }

      const GraphColorCell* cell = &valueStart[i];
      itemIndex = cell->c.itemIndex;
      *details = GraphMeterMode_scaleCellDetails(cell->c.details, scaleFactor);
   }
   /* fallthrough */

cellIsEmpty:
   if (y == 0)
      *details |= 0xC0;

   if (itemIndex == UINT8_MAX)
      return BAR_SHADOW;

   assert(itemIndex < maxItems);
   return Meter_attributes(this)[itemIndex];
}

static void GraphMeterMode_printCellDetails(uint8_t details) {
   if (details == 0x00) {
      // Use ASCII space instead. A braille blank character may display as a
      // substitute block and is less distinguishable from a cell with data.
      addch(' ');
      return;
   }
#ifdef HAVE_LIBNCURSESW
   if (CRT_utf8) {
      // Bits 3 and 4 of "details" might carry special meaning. When the whole
      // byte contains specific bit patterns, it indicates that only half cell
      // should be displayed in the ASCII display mode. The bits are supposed
      // to be filled in the Unicode display mode.
      if ((details & 0x9C) == 0x14 || (details & 0x39) == 0x28) {
         if (details == 0x14 || details == 0x28) { // Special case
            details = 0x18;
         } else {
            details |= 0x18;
         }
      }
      // Convert GraphColorCell.c.details bit representation to Unicode braille
      // dot ordering.
      //   (Bit0) a b (Bit3)  From:        h g f e d c b a (binary)
      //   (Bit1) c d (Bit4)               | | |  X   X  |
      //   (Bit2) e f (Bit5)               | | | | \ / | |
      //   (Bit6) g h (Bit7)               | | | |  X  | |
      //                      To: 0x2800 + h g f d b e c a
      // Braille Patterns [U+2800, U+28FF] in UTF-8: [E2 A0 80, E2 A3 BF]
      char sequence[4] = "\xE2\xA0\x80";
      // Bits 6 and 7 are in the second byte of the UTF-8 sequence.
      sequence[1] |= details >> 6;
      // Bits 0 to 5 are in the third byte.
      // The algorithm is optimized for x86 and ARM.
      uint32_t n = details * 0x01010101U;
      n = (uint32_t)((n & 0x08211204U) * 0x02110408U) >> 26;
      sequence[2] |= n;
      addstr(sequence);
      return;
   }
#endif
   // ASCII display mode
   const char upperHalf = '`';
   const char lowerHalf = '.';
   const char fullCell = ':';
   char c;

   // Detect special cases where we should print only half of the cell.
   if ((details & 0x9C) == 0x14) {
      c = upperHalf;
   } else if ((details & 0x39) == 0x28) {
      c = lowerHalf;
      // End of special cases
   } else if (popCount8(details) > 4) {
      c = fullCell;
   } else {
      // Determine which half has more dots than the other.
      uint8_t inverted = details ^ 0x0F;
      int difference = (int)popCount8(inverted) - 4;
      if (difference < 0) {
         c = upperHalf;
      } else if (difference > 0) {
         c = lowerHalf;
      } else {
         // Give weight to dots closer to the top or bottom of the cell (LSB or
         // MSB, respectively) as a tiebreaker.
         // Reverse bits 0 to 3 and subtract it from bits 4 to 7.
         // The algorithm is optimized for x86 and ARM.
         uint32_t n = inverted * 0x01010101U;
         n = (uint32_t)((n & 0xF20508U) * 0x01441080U) >> 27;
         difference = (int)n - 0x0F;
         c = difference < 0 ? upperHalf : lowerHalf;
      }
   }
   addch(c);
}

static void GraphMeterMode_draw(Meter* this, int x, int y, int w) {
   const char* caption = Meter_getCaption(this);
   attrset(CRT_colors[METER_TEXT]);
   const int captionLen = 3;
   mvaddnstr(y, x, caption, captionLen);

   assert(this->h >= 1);
   assert(this->h <= MAX_GRAPH_HEIGHT);
   unsigned int graphHeight = (unsigned int)this->h;

   uint8_t maxItems = Meter_maxItems(this);
   bool isPercentChart = Meter_isPercentChart(this);
   size_t nCellsPerValue = maxItems <= 1 ? maxItems : graphHeight;
   if (!isPercentChart)
      nCellsPerValue *= 2;

   GraphDrawContext context = {
      .maxItems = maxItems,
      .isPercentChart = isPercentChart,
      .nCellsPerValue = nCellsPerValue
   };

   bool needsScaleDisplay = maxItems > 0 && graphHeight >= 2;
   if (needsScaleDisplay) {
      move(y + 1, x); // Cursor position for printing the scale
   }
   x += captionLen;
   w -= captionLen;

   GraphData* data = &this->drawData;

   assert(data->nValues <= INT_MAX);
   if (w > (int)data->nValues && MAX_METER_GRAPHDATA_VALUES > data->nValues) {
      size_t nValues = data->nValues;
      nValues = MAXIMUM(nValues + nValues / 2, (size_t)w);
      nValues = MINIMUM(nValues, MAX_METER_GRAPHDATA_VALUES);
      GraphMeterMode_reallocateGraphBuffer(this, &context, nValues);
   }

   const size_t nValues = data->nValues;
   if (nValues < 1)
      return;

   const Machine* host = this->host;
   if (!timercmp(&host->realtime, &(data->time), <)) {
      int globalDelay = host->settings->delay;
      struct timeval delay = { .tv_sec = globalDelay / 10, .tv_usec = (globalDelay % 10) * 100000L };
      timeradd(&host->realtime, &delay, &(data->time));

      GraphMeterMode_recordNewValue(this, &context);
   }

   if (w <= 0)
      return;

   if ((size_t)w > nValues) {
      x += w - nValues;
      w = nValues;
   }

   size_t i = nValues - (size_t)w;

   int scaleExp = 0;
   if (maxItems > 0 && !isPercentChart) {
      for (unsigned int col = 0; i + col < nValues; col++) {
         const GraphColorCell* valueStart = &data->buffer[(i + col) * nCellsPerValue];
         if (scaleExp < valueStart[0].scaleExp) {
            scaleExp = valueStart[0].scaleExp;
         }
      }
   }
   if (needsScaleDisplay) {
      if (isPercentChart) {
         addstr("  %");
      } else {
         GraphMeterMode_printScale(scaleExp);
      }
   }

   for (unsigned int line = 0; line < graphHeight; line++) {
      for (unsigned int col = 0; i + col < nValues; col++) {
         uint8_t details;
         int colorIdx = GraphMeterMode_lookupCell(this, &context, scaleExp, i + col, line, &details);
         move(y + (int)line, x + (int)col);
         attrset(CRT_colors[colorIdx]);
         GraphMeterMode_printCellDetails(details);
      }
   }
   attrset(CRT_colors[RESET_COLOR]);
}

/* ---------- LEDMeterMode ---------- */

static const char* const LEDMeterMode_digitsAscii[] = {
   " __ ", "    ", " __ ", " __ ", "    ", " __ ", " __ ", " __ ", " __ ", " __ ",
   "|  |", "   |", " __|", " __|", "|__|", "|__ ", "|__ ", "   |", "|__|", "|__|",
   "|__|", "   |", "|__ ", " __|", "   |", " __|", "|__|", "   |", "|__|", " __|"
};

#ifdef HAVE_LIBNCURSESW

static const char* const LEDMeterMode_digitsUtf8[] = {
   "┌──┐", "  ┐ ", "╶──┐", "╶──┐", "╷  ╷", "┌──╴", "┌──╴", "╶──┐", "┌──┐", "┌──┐",
   "│  │", "  │ ", "┌──┘", " ──┤", "└──┤", "└──┐", "├──┐", "   │", "├──┤", "└──┤",
   "└──┘", "  ╵ ", "└──╴", "╶──┘", "   ╵", "╶──┘", "└──┘", "   ╵", "└──┘", "╶──┘"
};

#endif

static const char* const* LEDMeterMode_digits;

static void LEDMeterMode_drawDigit(int x, int y, int n) {
   for (int i = 0; i < 3; i++)
      mvaddstr(y + i, x, LEDMeterMode_digits[i * 10 + n]);
}

static void LEDMeterMode_draw(Meter* this, int x, int y, int w) {
#ifdef HAVE_LIBNCURSESW
   if (CRT_utf8)
      LEDMeterMode_digits = LEDMeterMode_digitsUtf8;
   else
#endif
      LEDMeterMode_digits = LEDMeterMode_digitsAscii;

   RichString_begin(out);
   Meter_displayBuffer(this, &out);

   int yText =
#ifdef HAVE_LIBNCURSESW
      CRT_utf8 ? y + 1 :
#endif
      y + 2;
   attrset(CRT_colors[LED_COLOR]);
   const char* caption = Meter_getCaption(this);
   mvaddstr(yText, x, caption);
   int xx = x + strlen(caption);
   int len = RichString_sizeVal(out);
   for (int i = 0; i < len; i++) {
      int c = RichString_getCharVal(out, i);
      if (c >= '0' && c <= '9') {
         if (xx - x + 4 > w)
            break;

         LEDMeterMode_drawDigit(xx, y, c - '0');
         xx += 4;
      } else {
         if (xx - x + 1 > w)
            break;
#ifdef HAVE_LIBNCURSESW
         const cchar_t wc = { .chars = { c, '\0' }, .attr = 0 }; /* use LED_COLOR from attrset() */
         mvadd_wch(yText, xx, &wc);
#else
         mvaddch(yText, xx, c);
#endif
         xx += 1;
      }
   }
   attrset(CRT_colors[RESET_COLOR]);
   RichString_delete(&out);
}

static MeterMode BarMeterMode = {
   .uiName = "Bar",
   .h = 1,
   .draw = BarMeterMode_draw,
};

static MeterMode TextMeterMode = {
   .uiName = "Text",
   .h = 1,
   .draw = TextMeterMode_draw,
};

static MeterMode GraphMeterMode = {
   .uiName = "Graph",
   .h = DEFAULT_GRAPH_HEIGHT,
   .draw = GraphMeterMode_draw,
};

static MeterMode LEDMeterMode = {
   .uiName = "LED",
   .h = 3,
   .draw = LEDMeterMode_draw,
};

const MeterMode* const Meter_modes[] = {
   NULL,
   &BarMeterMode,
   &TextMeterMode,
   &GraphMeterMode,
   &LEDMeterMode,
   NULL
};

/* Blank meter */

static void BlankMeter_updateValues(Meter* this) {
   this->txtBuffer[0] = '\0';
}

static void BlankMeter_display(ATTR_UNUSED const Object* cast, ATTR_UNUSED RichString* out) {
}

static const int BlankMeter_attributes[] = {
   DEFAULT_COLOR
};

const MeterClass BlankMeter_class = {
   .super = {
      .extends = Class(Meter),
      .delete = Meter_delete,
      .display = BlankMeter_display,
   },
   .updateValues = BlankMeter_updateValues,
   .defaultMode = TEXT_METERMODE,
   .maxItems = 0,
   .total = 100.0,
   .attributes = BlankMeter_attributes,
   .name = "Blank",
   .uiName = "Blank",
   .caption = ""
};
