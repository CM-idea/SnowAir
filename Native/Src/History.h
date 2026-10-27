#pragma once
#include <include/Ling.h>
class ShapeBase;
class AnnotHost;
// 视口滤镜（水印 / 包浆 / 高光）应覆盖的范围：标注坐标下的整张底图
D2D1_RECT_F annotImageRect(AnnotHost* win);
class History
{
public:
	History(AnnotHost* win);
	~History();
	ShapeBase* createShape(const std::wstring& state, const int& x, const int& y);
	// 选水印/包浆立即铺满选区
	void syncViewportFilters(const std::wstring& toolId);
	void undo();
	void redo();
	// 「清除」：清空当前画布上的**全部**标注。工具栏没有这颗按钮 —— 只有系统按键能触发，
	// 所以不走 ToolCap 的点击路由，直接在这里实现。
	void clearAll();
	bool canUndo() const;
	void removeHoverShape();
	void removeShape(ShapeBase* target);
	// 「整体删除」：箭头/线段与挂在它上面的线上文字算一个整体 —— 删其中任何一个，
	// 同组的另一个一起删（用户要的：选中整条线按 Delete，文字不能剩下）
	void removeShapeGroup(ShapeBase* target);
	void eraseAt(const float x, const float y);
	// 白板：调整背景尺寸也进撤销栈（page<0 = 纯色背景，否则为图片页码）
	void addBoardResize(int page, const D2D1_RECT_F& before, const D2D1_RECT_F& after);
	// 拖动/缩放/旋转已有标注：作为独立一步进撤销栈
	void addEditStep(ShapeBase* target, const std::vector<float>& before, const std::vector<float>& after);
	// 一次文字编辑会话（可能输入很多字）作为一步撤销；同时记录文字和几何
	void addTextEditStep(ShapeBase* target,
		const std::vector<float>& before, const std::vector<float>& after,
		const std::wstring& beforeText, const std::wstring& afterText);
	// 确认编辑：丢掉已撤销的 shape，释放内存并锁定本轮结果
	void purgeUndone();
	std::vector<std::unique_ptr<ShapeBase>> shapes;
private:
	void removeUndoShape();
	AnnotHost* win;
};
