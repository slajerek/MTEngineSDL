#ifndef _implot_colors_extension_h_
#define _implot_colors_extension_h_

namespace ImPlot {

IMPLOT_TMP void PlotStairsWithColorsExtension(const char* label_id, const T* values, int count, double xscale=1, double xstart=0, ImPlotStairsFlags flags=0, int offset=0, int stride=sizeof(T));
IMPLOT_TMP void PlotStairsDebugger(const char* label_id, const T* xs, const T* ys, int count, ImPlotStairsFlags flags=0, int offset=0, int stride=sizeof(T));

};

#endif //_implot_colors_extension_h_
