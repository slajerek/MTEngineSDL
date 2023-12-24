// Note: this extensions allows changing color of each stair in stairs plot via callback function
// it is a POC hack now, this should be refactored to a proper getter. works for now, but is a crude hack.
// RetroDebugger Note: this is a quick hack to allow us to render values of memory in memoryCell colors

#if defined(COLORS_EXTENSION_INCLUDED_FROM_IMPLOT)

// RetroDebugger Note: this is a crude hack to have a callback to get plot sample color
ImU32 ImPlotColorsExtensionGetterCallback(int index);

namespace ImPlot {

template <class _Getter>
struct RendererStairsWithColorsExtensionPost : RendererBase {
	RendererStairsWithColorsExtensionPost(const _Getter& getter, ImU32 col, float weight) :
		RendererBase(getter.Count - 1, 12, 8),
		Getter(getter),
		Col(col),
		HalfWeight(ImMax(1.0f,weight) * 0.5f)
	{
		P1 = this->Transformer(Getter(0));
	}
	void Init(ImDrawList& draw_list) const {
		UV = draw_list._Data->TexUvWhitePixel;
	}
	IMPLOT_INLINE bool Render(ImDrawList& draw_list, const ImRect& cull_rect, int prim) const {
//		LOGD("Render: prim=%d", prim);
		ImVec2 P2 = this->Transformer(Getter(prim + 1));
		if (!cull_rect.Overlaps(ImRect(ImMin(P1, P2), ImMax(P1, P2)))) {
			P1 = P2;
			return false;
		}
		
		ImU32 col = ImPlotColorsExtensionGetterCallback(prim);
		
		PrimRectFill(draw_list, ImVec2(P1.x, P1.y + HalfWeight), ImVec2(P2.x, P1.y - HalfWeight), col, UV);
		PrimRectFill(draw_list, ImVec2(P2.x - HalfWeight, P2.y), ImVec2(P2.x + HalfWeight, P1.y), col, UV);
		P1 = P2;
		return true;
	}
	const _Getter& Getter;
	const ImU32 Col;
	mutable float HalfWeight;
	mutable ImVec2 P1;
	mutable ImVec2 UV;
};

// RetroDebugger Note, this is a hack
template <typename Getter>
void PlotStairsWithColorsExtensionEx(const char* label_id, const Getter& getter, ImPlotStairsFlags flags) {
	if (BeginItemEx(label_id, Fitter1<Getter>(getter), flags, ImPlotCol_Line)) {
		if (getter.Count <= 0) {
			EndItem();
			return;
		}
		const ImPlotNextItemData& s = GetItemData();
		if (getter.Count > 1) {
			if (s.RenderFill && ImHasFlag(flags,ImPlotStairsFlags_Shaded)) {
				const ImU32 col_fill = ImGui::GetColorU32(s.Colors[ImPlotCol_Fill]);
				if (ImHasFlag(flags, ImPlotStairsFlags_PreStep))
					RenderPrimitives1<RendererStairsPreShaded>(getter,col_fill);
				else
					RenderPrimitives1<RendererStairsPostShaded>(getter,col_fill);
			}
			if (s.RenderLine) {
				const ImU32 col_line = ImGui::GetColorU32(s.Colors[ImPlotCol_Line]);
				if (ImHasFlag(flags, ImPlotStairsFlags_PreStep))
					RenderPrimitives1<RendererStairsPre>(getter,col_line,s.LineWeight);
				else
					RenderPrimitives1<RendererStairsWithColorsExtensionPost>(getter,col_line,s.LineWeight);
			}
		}
		// render markers
		if (s.Marker != ImPlotMarker_None) {
			PopPlotClipRect();
			PushPlotClipRect(s.MarkerSize);
			const ImU32 col_line = ImGui::GetColorU32(s.Colors[ImPlotCol_MarkerOutline]);
			const ImU32 col_fill = ImGui::GetColorU32(s.Colors[ImPlotCol_MarkerFill]);
			RenderMarkers<Getter>(getter, s.Marker, s.MarkerSize, s.RenderMarkerFill, col_fill, s.RenderMarkerLine, col_line, s.MarkerWeight);
		}
		EndItem();
	}
}

template <typename T>
void PlotStairsWithColorsExtension(const char* label_id, const T* values, int count, double xscale, double x0, ImPlotStairsFlags flags, int offset, int stride) {
	GetterXY<IndexerLin,IndexerIdx<T>> getter(IndexerLin(xscale,x0),IndexerIdx<T>(values,count,offset,stride),count);
	PlotStairsWithColorsExtensionEx(label_id, getter, flags);
}

template <typename T>
void PlotStairsWithColorsExtension(const char* label_id, const T* xs, const T* ys, int count, ImPlotStairsFlags flags, int offset, int stride) {
	GetterXY<IndexerIdx<T>,IndexerIdx<T>> getter(IndexerIdx<T>(xs,count,offset,stride),IndexerIdx<T>(ys,count,offset,stride),count);
	return PlotStairsWithColorsExtensionEx(label_id, getter, flags);
}

#define INSTANTIATE_MACRO(T) \
	template IMPLOT_API void PlotStairsWithColorsExtension<T> (const char* label_id, const T* values, int count, double xscale, double x0, ImPlotStairsFlags flags, int offset, int stride); \
	template IMPLOT_API void PlotStairsWithColorsExtension<T>(const char* label_id, const T* xs, const T* ys, int count, ImPlotStairsFlags flags, int offset, int stride);
CALL_INSTANTIATE_FOR_NUMERIC_TYPES()
#undef INSTANTIATE_MACRO

};

#endif
