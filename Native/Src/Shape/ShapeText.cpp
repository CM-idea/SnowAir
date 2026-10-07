#include "pch.h"
#include "App.h"
#include "Util.h"
#include "History.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "ShapeText.h"

using Microsoft::WRL::ComPtr;

ShapeText::ShapeText(AnnotHost* win) : ShapeBase(win),
	borderPadding{ 6.f * win->dpi },
	draggers(4, D2D1::RectF(0, 0, 0, 0))
{
	setAttr();
	float dashes[] = { 2.f, 2.f };
	Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
		dashes, ARRAYSIZE(dashes), dashedStrokeStyle.GetAddressOf());
}

ShapeText::~ShapeText() {}

void ShapeText::updateDraggers()
{
	const float half = draggerSize * 0.5f;
	draggers[0] = D2D1::RectF(rect.left - half, rect.top - half, rect.left + half, rect.top + half);
	draggers[1] = D2D1::RectF(rect.right - half, rect.top - half, rect.right + half, rect.top + half);
	draggers[2] = D2D1::RectF(rect.left - half, rect.bottom - half, rect.left + half, rect.bottom + half);
	draggers[3] = D2D1::RectF(rect.right - half, rect.bottom - half, rect.right + half, rect.bottom + half);
}

void ShapeText::syncRectFromLayout()
{
	if (!textLayout) return;
	DWRITE_TEXT_METRICS m{};
	textLayout->GetMetrics(&m);
	rect.right = rect.left + m.widthIncludingTrailingWhitespace + borderPadding * 2.f;
	// 线上文字（行距被压过）要把最后一行下伸的那点余量补进高度，否则洞/编辑框比字矮
	rect.bottom = rect.top + m.height + (attachTo ? attachLineSlack : 0.f) + borderPadding * 2.f;
	updateDraggers();
}

void ShapeText::paint(ID2D1DeviceContext* ctx)
{
	if (isEditing) {
		// 附着在线上时不镜像输入框：洞要用"提交后那套几何"（= setAttachPose 算出的
		// 以锚点为中心、实测文字尺寸 + 两侧间距），这样编辑中和提交后看起来是同一个洞，
		// 不会出现"编辑时窄高、退出后正常"的跳变。非附着文字保持原来的镜像行为。
		if (!attachTo) {
			auto tb = win->getTextBox();
			auto tl = win->clientToAnnotPt(tb->x, tb->y);
			auto br = win->clientToAnnotPt(tb->x + tb->w, tb->y + tb->h);
			rect = D2D1::RectF(tl.x, tl.y, br.x, br.y);
		}
		updateDraggers();
		paintPunch(ctx);   // 编辑中也要开洞，不然文字框里还压着线
		return;
	}
	if (!textLayout) return;
	paintPunch(ctx);
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	// 普通文字可以自己拖角度（四角外一圈旋转）；线上文字角度恒为 0（一律横排）
	if (angle != 0.f) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		ctx->SetTransform(D2D1::Matrix3x2F::Rotation(angle * 180.f / 3.14159265f, { cx, cy }) * old);
	}
	ctx->DrawTextLayout({ rect.left + borderPadding, rect.top + borderPadding },
		textLayout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	ctx->SetTransform(old);
}

// 挂到箭头/线段上：只把文字**中心**摆在线段中点，**不跟线转**。
// 「线上文字一律横排」是用户定的：不管箭头是左→右、右→左还是垂直，文字方向都一样（永远
// 正着读，不会镜像/倒置）；要竖着排就自己在编辑框里按回车换行（一个字符一行）。
// 所以这里把 angle 归零 —— 不这么做的话，右→左的箭头会把文字转 180° 倒过来、垂直的箭头
// 会转 90° 变成"躺着读"
void ShapeText::setAttachPose(float cx, float cy)
{
	if (!win) return;
	angle = 0.f;
	attachCx = cx;
	attachCy = cy;
	float w = 0.f, h = 0.f;
	if (textLayout) {
		DWRITE_TEXT_METRICS m{};
		textLayout->GetMetrics(&m);
		w = m.widthIncludingTrailingWhitespace + borderPadding * 2.f;
		// 行距压掉的那点余量要补回来（最后一行的下伸在排版高度之外），
		// 不然洞会比字矮一截、线从那行的尾巴上压过去
		h = m.height + attachLineSlack + borderPadding * 2.f;
	}
	if (w < 1.f || h < 1.f) {   // 还没内容：洞 = 一个字 + 两侧间距，高度就是一行
		// （这里曾经借"一个字"跑一次真排版来量尺寸，还带上下限兜底 —— 那套已经被
		//  syncEditorGeometry + 一行高编辑框取代了：洞的尺寸不再影响插入符位置）
		w = fontSize + borderPadding * 2.f;
		h = fontSize * 1.35f + borderPadding * 2.f;
	}
	rect = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
	// 取整到整像素：提交后是用浮点 rect 画文字/洞，编辑中是把 rect 换算成 client 像素摆输入框，
	// 两条路径差 0.5~1px 就是这么来的。这里统一吸附到整像素，两边就完全对齐了
	rect.left = std::floor(rect.left);
	rect.top = std::floor(rect.top);
	rect.right = std::floor(rect.right);
	rect.bottom = std::floor(rect.bottom);
	updateDraggers();
}

void ShapeText::paintPunch(ID2D1DeviceContext* ctx){
	// 「线段中间打字」：用底图把文字这一块抠回来 —— 线段/箭头从文字下穿过就会断成两截，
	// 正好是尺寸标注那种"文字压在线上但线不穿字"的样子。没开这个标记的文字不受影响。
	// 编辑中也要挖洞：现在编辑与提交共用同一套几何（rect = setAttachPose 的锚点居中几何），
	// 洞在编辑中就是提交后那个洞，不会再出现"编辑时挖歪"
	if (!punchBg || !win || !win->screenImg || !ctx) return;
	if (rect.right - rect.left < 1.f || rect.bottom - rect.top < 1.f) return;
	// 洞贴着字就行：别用 borderPadding（那是编辑框的留白），不然洞比字宽一圈
	const float pad = 2.f * (win ? win->dpi : 1.f);
	const D2D1_RECT_F r = D2D1::RectF(rect.left - pad, rect.top - pad, rect.right + pad, rect.bottom + pad);
	// 线上文字一律横排（angle==0），洞就是轴对齐的一块：多行文字（用户自己回车的竖列）
	// 也整块抠掉，线在整列文字的上下/左右各留 pad 的间距
	ctx->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
	ctx->DrawBitmap(win->screenImg.Get(), D2D1::RectF(0.f, 0.f, win->w, win->h));
	ctx->PopAxisAlignedClip();
}

void ShapeText::paintDragger(ID2D1DeviceContext* ctx)
{
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	if (angle != 0.f) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		ctx->SetTransform(D2D1::Matrix3x2F::Rotation(angle * 180.f / 3.14159265f, { cx, cy }) * old);
	}
	ctx->DrawRectangle(rect, brushHandleBorder.Get(), win->dpi, dashedStrokeStyle.Get());
	for (int i = 0; i < 4; i++) paintHandle(ctx, draggers[i]);
	ctx->SetTransform(old);
}

void ShapeText::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 8) {
		rect.left += x - pressX;
		rect.right += x - pressX;
		rect.top += y - pressY;
		rect.bottom += y - pressY;
		pressX = x;
		pressY = y;
		updateDraggers();
		return;
	}
	if (hoverDraggerIndex >= 0 && hoverDraggerIndex <= 3) {
		const float cx = (resizeStartRect.left + resizeStartRect.right) * 0.5f;
		const float cy = (resizeStartRect.top + resizeStartRect.bottom) * 0.5f;
		const float dx0 = std::max(1.f, std::hypot(pressX - cx, pressY - cy));
		const float dx1 = std::max(1.f, std::hypot(x - cx, y - cy));
		const float minPx = 8.f * win->dpi;
		const float maxPx = 200.f * win->dpi;
		fontSize = std::clamp(resizeStartFont * (dx1 / dx0), minPx, maxPx);
		makeTextLayout();
		syncRectFromLayout();
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		rect.left = cx - w * 0.5f;
		rect.right = cx + w * 0.5f;
		rect.top = cy - h * 0.5f;
		rect.bottom = cy + h * 0.5f;
		updateDraggers();
		// 拖角缩放时实时同步属性条数值
		if (win->toolSub) win->toolSub->setShapeSliderVal(L"text", fontSize);
		return;
	}
	if (hoverDraggerIndex == 4) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		angle = rotateStartAngle + (std::atan2(y - cy, x - cx) - rotateStartAtan);
		updateDraggers();
	}
}

void ShapeText::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) {
		rect = D2D1::RectF(x - borderPadding, y - borderPadding, x + borderPadding, y + borderPadding);
		hoverDraggerIndex = 9;
		startEdit();
		return;
	}
	if (hoverDraggerIndex == 9) {
		pressX = x;
		pressY = y;
		return;
	}
	if (hoverDraggerIndex == 8) {
		pressX = x;
		pressY = y;
		if (isEditing) finishEdit();
		return;
	}
	if (hoverDraggerIndex >= 0 && hoverDraggerIndex <= 3) {
		if (isEditing) finishEdit();
		pressX = x;
		pressY = y;
		resizeStartFont = fontSize;
		resizeStartRect = rect;
		return;
	}
	if (hoverDraggerIndex == 4) {
		if (isEditing) finishEdit();
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		rotateStartAngle = angle;
		rotateStartAtan = std::atan2(y - cy, x - cx);
	}
}

void ShapeText::mouseUp(const float x, const float y)
{
	(void)x; (void)y;
	updateDraggers();
}

void ShapeText::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	updateDraggers();
	// 四角外侧一圈 = 旋转（角上仍是缩放）；不再有单独旋转柄
	float lx = x, ly = y;
	if (std::fabs(angle) > 1e-6f) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		const float dx = x - cx, dy = y - cy;
		const float ca = std::cos(-angle), sa = std::sin(-angle);
		lx = cx + dx * ca - dy * sa;
		ly = cy + dx * sa + dy * ca;
	}
	const int rotCorner = isSelected() ? hitRotateRing(lx, ly, rect) : -1;
	if (rotCorner >= 0) {
		hoverDraggerIndex = 4;
		rotateCorner = rotCorner;
		return;
	}
	// 四角优先；四边也可调（映射到对应角，缩放逻辑相同）
	const float band = std::max(8.f * win->dpi, borderPadding);
	const int z = hitBoxEdgeOrCorner(lx, ly, rect, band);
	if (z >= 0) {
		// Area 索引 → Text 四角：0=TL 2=TR 4=BR 6=BL → 0,1,3,2
		if (z == 0 || z == 1 || z == 7) hoverDraggerIndex = 0;
		else if (z == 2 || z == 3) hoverDraggerIndex = 1;
		else if (z == 4 || z == 5) hoverDraggerIndex = 3;
		else hoverDraggerIndex = 2;
		return;
	}
	const float half = borderPadding / 2.f + win->dpi;
	if (x >= rect.left - half && x <= rect.right + half && y >= rect.top - half && y <= rect.bottom + half) {
		// 内部：编辑中为输入，已选中为平移
		hoverDraggerIndex = isEditing ? 9 : 8;
		return;
	}
	if (isEditing) hoverDraggerIndex = 0;
}

void ShapeText::setCursor()
{
	// 文本工具在任何文字上（尤其是挂在线上那种）：始终工字型 ——
	// 否则已选中的文字会走移动/缩放光标（↔），二次编辑时鼠标就变了样
	if (win && win->getCurToolId() == L"text") {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
		return;
	}
	if (applyUnselectedHoverCursor()) return;
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 3)
		SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
	else if (hoverDraggerIndex == 1 || hoverDraggerIndex == 2)
		SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
	else if (hoverDraggerIndex == 4)
		setRotateCursor(rotateCorner, angle);
	else if (hoverDraggerIndex == 8)
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	else if (hoverDraggerIndex == 9)
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	else
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapeText::hitErase(const float x, const float y)
{
	return hitLabelBox(x, y);
}

// 命中"这块文字"（含它周围一点边距）。线上文字一律横排（angle==0），普通文字可能是
// 用户自己转过角度的：那就先把点转回文字自己的坐标系再判，否则"两端点不到、左右误命中"
// （双击线上文字进编辑、橡皮擦擦文字都靠它）
bool ShapeText::hitLabelBox(const float x, const float y) const
{
	const float half = borderPadding / 2.f + (win ? win->dpi : 1.f);
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;
	float lx = x - cx, ly = y - cy;
	if (std::fabs(angle) > 1e-6f) {
		// 逆变换（屏幕 → 文字自己的坐标系）：D2D 的 Rotation(θ) 是 [c -s; s c]（y 向下、顺时针为正）
		const float c = std::cos(angle), s = std::sin(angle);
		const float u = lx * c + ly * s;
		const float v = -lx * s + ly * c;
		lx = u;
		ly = v;
	}
	const float halfW = (rect.right - rect.left) * 0.5f + half;
	const float halfH = (rect.bottom - rect.top) * 0.5f + half;
	return std::fabs(lx) <= halfW && std::fabs(ly) <= halfH;
}

// 把输入框摆到当前 rect 上。附着文字的 rect 是"以线段锚点为中心"算出来的，
// 所以调用它 = 把插入符挪到线段中间（而不是落笔那个鼠标位置）
void ShapeText::syncEditorGeometry()
{
	if (!isEditing || !win) return;
	auto tb = win->getTextBox();
	auto d = win->dpi;
	auto tl = win->annotToClientPt(rect.left, rect.top);
	auto br = win->annotToClientPt(rect.right, rect.bottom);
	tb->setPosition(Ling::Edge::Left, tl.x / d);
	tb->setPosition(Ling::Edge::Top, tl.y / d);
	tb->x = tl.x;
	tb->y = tl.y;
	tb->w = std::max(1.f, br.x - tl.x);
	tb->h = std::max(1.f, br.y - tl.y);
	const float visScale = (rect.right > rect.left) ? (tb->w / (rect.right - rect.left)) : win->scale;
	tb->setPadding(attachTo ? 0.f : borderPadding * visScale / d);
	// 附着文字：让输入框把文字**垂直居中**排在自己的框里 —— 这才是"插入符上下居中"的正解。
	// Ling::TextBox 默认从框顶往下排，框比一行高时插入符就偏上/偏下；之前我用"整体上抬
	// borderPadding"去硬凑，那只是把偏差挪了个方向，已经删掉
	// 附着文字：输入框**自己的背景要透明**。编辑中你看到的那个"刚好贴着文字、没有两边间距"
	// 的缝，其实是输入框的不透明底色把线擦掉的（不是我们挖的洞）—— 洞是 paintPunch 按
	// rect（文字 + 两侧间距、以锚点居中）挖的，被它盖在上面挡住了。
	// 同时让它按内容自适应：这样"以锚点为中心"才是真的把文字居中，插入符也才会落在洞里。
	if (attachTo) {
		tb->setBg(0);
		// 输入框是 autoSize 的（AnnotHost::getTextBox 建框时就开了）：框 = 文字大小，
		// 文字从**框的左上角**开始排。所以"把框居中"≠"把文字居中" —— 文字会整体偏到右下方，
		// 插入符也跟着跑到右边（你截图里那个贴着文字、没两边间距的缝就是它）。
		// 正解：把框往左上挪半个**文字**尺寸，让文字自己以线段锚点为中心。
		float tw = std::max(1.f, rect.right - rect.left - borderPadding * 2.f);
		float th = std::max(1.f, rect.bottom - rect.top - borderPadding * 2.f);
		if (textLayout) {
			DWRITE_TEXT_METRICS m{};
			textLayout->GetMetrics(&m);
			if (m.widthIncludingTrailingWhitespace > 0.5f) tw = m.widthIncludingTrailingWhitespace;
			// 高度同样带上行距压掉的那点（和 setAttachPose/syncRectFromLayout 一致），
			// 输入框和落下后的文字才是同一个盒、行尾也不会被裁
			if (m.height > 0.5f) th = m.height + attachLineSlack;
		}
		else {
			// 空内容：插入符就在文字起点，把"文字宽"当 0 → 框左边正好落在锚点上，
			// 插入符也就压在线段中点（框宽给个极小值就行，别从左边开始排导致偏左）
			tw = 0.f;
		}
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		const auto tlp = win->annotToClientPt(cx - tw * 0.5f, cy - th * 0.5f);
		tb->x = tlp.x;
		tb->y = tlp.y;
		tb->w = std::max(1.f, tw * win->scale);
		tb->h = std::max(1.f, th * win->scale);
		tb->setPosition(Ling::Edge::Left, tlp.x / d);
		tb->setPosition(Ling::Edge::Top, tlp.y / d);
	}
	tb->setVerticalCenter(false);
	// 行距跟着 commit 后的排版走（线上文字一行一个字，见 makeTextLayout）：
	// 输入框里也得是同一个行距，不然打字时和落下后高度不一样
	tb->setLineSpacing(attachTo ? attachLineStep * visScale / d : 0.f);
}

void ShapeText::startEdit()
{
	if (isEditing) return;
	isEditing = true;
	// 不用 setAttr()：会把拖大的字号冲成工具条滑条值
	auto tb = win->getTextBox();
	auto d = win->dpi;
	auto tl = win->annotToClientPt(rect.left, rect.top);
	auto br = win->annotToClientPt(rect.right, rect.bottom);
	// 挂在箭头上（还没打字）：输入框只给"一行高、以线段中点为中心"。之前给它的是"行高 +
	// 两侧内边距"的框，输入框自己的内边距再叠上去，插入符就排到线段下方去了（截图里那根
	// 竖线）；一行高之后它没有余量可以往下沉，插入符自然压在线上
	if (attachTo) {
		const float halfW = (br.x - tl.x) * 0.5f;
		const float cx = tl.x + halfW;
		const float cy = (tl.y + br.y) * 0.5f;
		const float lineH = std::max(1.f, (float)(fontSize * d * 1.35));
		tl.x = cx - halfW;
		br.x = cx + halfW;
		tl.y = cy - lineH * 0.5f;
		br.y = cy + lineH * 0.5f;
	}
	tb->setPosition(Ling::Edge::Left, tl.x / d);
	tb->setPosition(Ling::Edge::Top, tl.y / d);
	tb->x = tl.x;
	tb->y = tl.y;
	tb->w = std::max(1.f, br.x - tl.x);
	tb->h = std::max(1.f, br.y - tl.y);
	tb->setColor(Ling::Color(colorValue));
	tb->setCaretColor(Ling::Color(colorValue));
	const float visScale = (rect.right > rect.left)
		? (tb->w / (rect.right - rect.left)) : win->scale;
	// 附着在箭头上的文字：编辑框**不留内边距**。框高已经是一行，再叠一份内边距，文字
	// 就被挤到框的下半部分（= 截图里插入符沉在线段下方）。参考实现（snow-apps-main）里
	// 附着文字是"中心锚点 + 用同一个排版量出的尺寸"，没有第二份留白可叠。
	tb->setPadding(attachTo ? 0.f : borderPadding * visScale / d);
	tb->setFontSize(fontSize * visScale / d);
	tb->setBold(isBold);
	tb->setItalic(isItalic);
	tb->setText(text);
	tb->show();
	textChangedTok = tb->onTextChanged.add([this](Ling::TextBox*, const std::wstring& val) {
		text = val;
		// 线上文字：一边打字一边按"线段中点"重新居中。否则字是从左边长出来的 ——
		// 编辑中看着偏左，提交后才"跳"到中间
		if (attachTo) {
			// 一边打字一边按线段锚点重新居中：重排版 → 重算锚点几何 → 输入框跟过去
			// （和落笔时那句 syncEditorGeometry 是同一个动作，避免两份几何代码各改一半）
			makeTextLayout();
			setAttachPose(attachCx, attachCy);
			syncEditorGeometry();
		}
		win->refresh();
	});
	focusTok = tb->onFocusChanged.add([this](Ling::TextBox*, bool focused) {
		if (!focused) finishEdit();
	});
	win->setEditingText(this);
	tb->focus();
	// 开编辑这条路径也要走同一套几何（syncEditorGeometry 里含"附着文字上抬"），
	// 否则从落笔进入编辑时插入符又会沉到洞的下半部分
	syncEditorGeometry();
	// startEdit 只由"这一次鼠标按下"触发（落笔建文字 / 双击线上文字 / 点已有标签），而上面
	// syncEditorGeometry 刚把输入框挪到锚点：紧接着同一次按下的 TextBox::onDown 还会跑一遍，
	// 它用的是**挪动前**的几何 —— 空标签的框只有 1px 宽，于是被判成"点在框外"→失焦→
	// finishEdit（空文字还会被异步删掉），表现就是"第一个箭头能打字，第二个点上去插不进光标"。
	// 让输入框对这一次按下豁免"框外失焦"（下一次按下前会自动清掉，见 TextBox::onUp）
	tb->holdFocusForHostPress();
	win->refresh();
}

void ShapeText::finishEdit()
{
	if (!isEditing) return;
	isEditing = false;
	auto tb = win->getTextBox();
	tb->onTextChanged.remove(textChangedTok);
	tb->onFocusChanged.remove(focusTok);
	textChangedTok = {};
	focusTok = {};
	text = tb->getText();
	tb->blur();
	tb->hide();
	win->setEditingText(nullptr);
	makeTextLayout();
	syncRectFromLayout();
	// 失焦/点空白结束后，吞掉紧随的点击，避免立刻又选中并画出控件
	win->annotSuppressUntil = GetTickCount64() + 400;
	win->shapeHover = nullptr;
	win->refresh();
	if (text.empty()) {
		Ling::App::get()->dq.TryEnqueue([w = win, self = this]() {
			w->history->removeShape(self);
		});
	}
}

void ShapeText::applyStyle()
{
	setAttr();
	if (isEditing) {
		auto tb = win->getTextBox();
		tb->setColor(Ling::Color(colorValue));
		tb->setCaretColor(Ling::Color(colorValue));
		tb->setFontSize(fontSize * win->scale / win->dpi);
		tb->setBold(isBold);
		tb->setItalic(isItalic);
	}
	else {
		makeTextLayout();
		syncRectFromLayout();
	}
	win->refresh();
}

void ShapeText::makeTextLayout()
{
	textLayout = Ling::D2D::get()->makeTextLayout(text, fontSize);
	if (!textLayout) return;
	textLayout->SetFontWeight(isBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, { 0, INT_MAX });
	textLayout->SetFontStyle(isItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, { 0, INT_MAX });
	// 线上文字：行距压到"一个字" —— 用户是"自己回车换行来竖排"的（一个字符一行），
	// 用字体默认行高（约 1.35 字）会把字与字的上下间距拉得比横排时的左右间距宽。
	// 行距取**字体自己的 baseline（≈ 一个字）**：再小的话第一行的上伸就会顶出行框
	// （被内容区裁剪切掉）；取行度量必须**两段式** —— GetLineMetrics(&lm, 1, &n) 在
	// 多于一行的排版上会直接失败返回全 0，拿 0 当 baseline 传下去字会被顶到行框上方，
	// 表现就是"打字时字被裁 + 上下间距不统一"（2026-09-26 踩过）
	if (attachTo) {
		attachLineStep = 0.f;
		attachLineSlack = 0.f;
		UINT32 lineCount = 0;
		textLayout->GetLineMetrics(nullptr, 0, &lineCount);
		if (lineCount == 0) lineCount = 1;
		std::vector<DWRITE_LINE_METRICS> lines(lineCount);
		UINT32 got = 0;
		if (SUCCEEDED(textLayout->GetLineMetrics(lines.data(), lineCount, &got)) && got > 0) {
			attachLineStep = lines[0].baseline;
			textLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, attachLineStep, lines[0].baseline);
			attachLineSlack = lines[0].height > attachLineStep ? lines[0].height - attachLineStep : 0.f;
		}
	}
}

void ShapeText::setAttr()
{
	auto toolSub = win->toolSub.get();
	colorValue = toolSub->getSelectedColorValue();
	color = toolSub->getSelectedColor();
	fontSize = toolSub->getSliderVal();
	isBold = toolSub->isTextBold;
	isItalic = toolSub->isTextItalic;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, textBrush.ReleaseAndGetAddressOf());
}
