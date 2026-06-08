#include "ui/RangeSlider.h"

namespace {
const int scHandleSideLength = 11;
const int scSliderBarHeight  = 5;
const int scLeftRightMargin  = 1;
}

RangeSlider::RangeSlider(QWidget* aParent)
	: QWidget(aParent)
	, mMinimum(0), mMaximum(100)
	, mLowerValue(0), mUpperValue(100)
	, mFirstHandlePressed(false), mSecondHandlePressed(false)
	, mInterval(100)
	, mBackgroudColorEnabled(QColor(0x1E, 0x90, 0xFF))
	, mBackgroudColorDisabled(Qt::darkGray)
	, mBackgroudColor(mBackgroudColorEnabled)
	, orientation(Qt::Horizontal)
	, type(DoubleHandles)
{
	setMouseTracking(true);
}

RangeSlider::RangeSlider(Qt::Orientation ori, Options t, QWidget* aParent)
	: QWidget(aParent)
	, mMinimum(0), mMaximum(100)
	, mLowerValue(0), mUpperValue(100)
	, mFirstHandlePressed(false), mSecondHandlePressed(false)
	, mInterval(100)
	, mBackgroudColorEnabled(QColor(0x1E, 0x90, 0xFF))
	, mBackgroudColorDisabled(Qt::darkGray)
	, mBackgroudColor(mBackgroudColorEnabled)
	, orientation(ori)
	, type(t)
{
	setMouseTracking(true);
}

void RangeSlider::paintEvent(QPaintEvent* aEvent)
{
	Q_UNUSED(aEvent)
	QPainter painter(this);

	QRectF backgroundRect;
	if (orientation == Qt::Horizontal)
		backgroundRect = QRectF(scLeftRightMargin, (height() - scSliderBarHeight) / 2,
		                        width() - scLeftRightMargin * 2, scSliderBarHeight);
	else
		backgroundRect = QRectF((width() - scSliderBarHeight) / 2, scLeftRightMargin,
		                        scSliderBarHeight, height() - scLeftRightMargin * 2);

	QPen pen(Qt::gray, 0.8);
	painter.setPen(pen);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setBrush(QColor(0xD0, 0xD0, 0xD0));
	painter.drawRoundedRect(backgroundRect, 1, 1);

	pen.setColor(Qt::darkGray);
	pen.setWidth(0.5);
	painter.setPen(pen);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setBrush(QColor(0xFA, 0xFA, 0xFA));

	QRectF leftHandleRect = firstHandleRect();
	if (type.testFlag(LeftHandle))
		painter.drawRoundedRect(leftHandleRect, 2, 2);

	QRectF rightHandleRect = secondHandleRect();
	if (type.testFlag(RightHandle))
		painter.drawRoundedRect(rightHandleRect, 2, 2);

	painter.setRenderHint(QPainter::Antialiasing, false);
	QRectF selectedRect(backgroundRect);
	if (orientation == Qt::Horizontal) {
		selectedRect.setLeft((type.testFlag(LeftHandle) ? leftHandleRect.right() : leftHandleRect.left()) + 0.5);
		selectedRect.setRight((type.testFlag(RightHandle) ? rightHandleRect.left() : rightHandleRect.right()) - 0.5);
	} else {
		selectedRect.setTop((type.testFlag(LeftHandle) ? leftHandleRect.bottom() : leftHandleRect.top()) + 0.5);
		selectedRect.setBottom((type.testFlag(RightHandle) ? rightHandleRect.top() : rightHandleRect.bottom()) - 0.5);
	}
	painter.setBrush(mBackgroudColor);
	painter.drawRect(selectedRect);
}

QRectF RangeSlider::firstHandleRect() const
{
	float pct = (mLowerValue - mMinimum) * 1.0f / mInterval;
	return handleRect(int(pct * validLength()) + scLeftRightMargin);
}

QRectF RangeSlider::secondHandleRect() const
{
	float pct = (mUpperValue - mMinimum) * 1.0f / mInterval;
	return handleRect(int(pct * validLength()) + scLeftRightMargin
	                  + (type.testFlag(LeftHandle) ? scHandleSideLength : 0));
}

QRectF RangeSlider::handleRect(int aValue) const
{
	if (orientation == Qt::Horizontal)
		return QRect(aValue, (height() - scHandleSideLength) / 2, scHandleSideLength, scHandleSideLength);
	return QRect((width() - scHandleSideLength) / 2, aValue, scHandleSideLength, scHandleSideLength);
}

void RangeSlider::mousePressEvent(QMouseEvent* aEvent)
{
	if (!(aEvent->buttons() & Qt::LeftButton)) return;

	int posCheck = (orientation == Qt::Horizontal) ? aEvent->pos().y() : aEvent->pos().x();
	int posMax   = (orientation == Qt::Horizontal) ? height() : width();
	int posValue = (orientation == Qt::Horizontal) ? aEvent->pos().x() : aEvent->pos().y();
	int firstPos  = (orientation == Qt::Horizontal) ? int(firstHandleRect().x())  : int(firstHandleRect().y());
	int secondPos = (orientation == Qt::Horizontal) ? int(secondHandleRect().x()) : int(secondHandleRect().y());

	mSecondHandlePressed = secondHandleRect().contains(aEvent->pos());
	mFirstHandlePressed  = !mSecondHandlePressed && firstHandleRect().contains(aEvent->pos());

	if (mFirstHandlePressed)
		mDelta = posValue - (firstPos + scHandleSideLength / 2);
	else if (mSecondHandlePressed)
		mDelta = posValue - (secondPos + scHandleSideLength / 2);

	if (posCheck >= 2 && posCheck <= posMax - 2) {
		int step = mInterval / 10 < 1 ? 1 : mInterval / 10;
		if (posValue < firstPos)
			setLowerValue(mLowerValue - step);
		else if (((posValue > firstPos + scHandleSideLength) || !type.testFlag(LeftHandle))
		      && ((posValue < secondPos)                     || !type.testFlag(RightHandle))) {
			if (type.testFlag(DoubleHandles)) {
				if (posValue - (firstPos + scHandleSideLength) <
				    (secondPos - (firstPos + scHandleSideLength)) / 2)
					setLowerValue(mLowerValue + step < mUpperValue ? mLowerValue + step : mUpperValue);
				else
					setUpperValue(mUpperValue - step > mLowerValue ? mUpperValue - step : mLowerValue);
			} else if (type.testFlag(LeftHandle))
				setLowerValue(mLowerValue + step < mUpperValue ? mLowerValue + step : mUpperValue);
			else if (type.testFlag(RightHandle))
				setUpperValue(mUpperValue - step > mLowerValue ? mUpperValue - step : mLowerValue);
		} else if (posValue > secondPos + scHandleSideLength)
			setUpperValue(mUpperValue + step);
	}
}

void RangeSlider::mouseMoveEvent(QMouseEvent* aEvent)
{
	if (!(aEvent->buttons() & Qt::LeftButton)) return;

	int posValue  = (orientation == Qt::Horizontal) ? aEvent->pos().x()     : aEvent->pos().y();
	int firstPos  = (orientation == Qt::Horizontal) ? int(firstHandleRect().x())  : int(firstHandleRect().y());
	int secondPos = (orientation == Qt::Horizontal) ? int(secondHandleRect().x()) : int(secondHandleRect().y());

	if (mFirstHandlePressed && type.testFlag(LeftHandle)) {
		if (posValue - mDelta + scHandleSideLength / 2 <= secondPos)
			setLowerValue(int((posValue - mDelta - scLeftRightMargin - scHandleSideLength / 2) * 1.0 / validLength() * mInterval) + mMinimum);
		else
			setLowerValue(mUpperValue);
	} else if (mSecondHandlePressed && type.testFlag(RightHandle)) {
		if (firstPos + scHandleSideLength * (type.testFlag(DoubleHandles) ? 1.5 : 0.5) <= posValue - mDelta)
			setUpperValue(int((posValue - mDelta - scLeftRightMargin - scHandleSideLength / 2
			             - (type.testFlag(DoubleHandles) ? scHandleSideLength : 0)) * 1.0 / validLength() * mInterval) + mMinimum);
		else
			setUpperValue(mLowerValue);
	}
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* aEvent)
{
	Q_UNUSED(aEvent)
	mFirstHandlePressed = mSecondHandlePressed = false;
}

void RangeSlider::changeEvent(QEvent* aEvent)
{
	if (aEvent->type() == QEvent::EnabledChange) {
		mBackgroudColor = isEnabled() ? mBackgroudColorEnabled : mBackgroudColorDisabled;
		update();
	}
}

QSize RangeSlider::minimumSizeHint() const
{
	return QSize(scHandleSideLength * 2 + scLeftRightMargin * 2, scHandleSideLength);
}

int  RangeSlider::GetMinimun()    const { return mMinimum; }
int  RangeSlider::GetMaximun()    const { return mMaximum; }
int  RangeSlider::GetLowerValue() const { return mLowerValue; }
int  RangeSlider::GetUpperValue() const { return mUpperValue; }

void RangeSlider::SetMinimum(int v)    { setMinimum(v); }
void RangeSlider::SetMaximum(int v)    { setMaximum(v); }
void RangeSlider::SetLowerValue(int v) { setLowerValue(v); }
void RangeSlider::SetUpperValue(int v) { setUpperValue(v); }

void RangeSlider::setLowerValue(int aLowerValue)
{
	mLowerValue = qBound(mMinimum, aLowerValue, mMaximum);
	emit lowerValueChanged(mLowerValue);
	update();
}

void RangeSlider::setUpperValue(int aUpperValue)
{
	mUpperValue = qBound(mMinimum, aUpperValue, mMaximum);
	emit upperValueChanged(mUpperValue);
	update();
}

void RangeSlider::setMinimum(int aMinimum)
{
	if (aMinimum <= mMaximum) mMinimum = aMinimum;
	else { int old = mMaximum; mMinimum = old; mMaximum = aMinimum; }
	mInterval = mMaximum - mMinimum;
	update();
	setLowerValue(mMinimum);
	setUpperValue(mMaximum);
	emit rangeChanged(mMinimum, mMaximum);
}

void RangeSlider::setMaximum(int aMaximum)
{
	if (aMaximum >= mMinimum) mMaximum = aMaximum;
	else { int old = mMinimum; mMaximum = old; mMinimum = aMaximum; }
	mInterval = mMaximum - mMinimum;
	update();
	setLowerValue(mMinimum);
	setUpperValue(mMaximum);
	emit rangeChanged(mMinimum, mMaximum);
}

int RangeSlider::validLength() const
{
	int len = (orientation == Qt::Horizontal) ? width() : height();
	return len - scLeftRightMargin * 2 - scHandleSideLength * (type.testFlag(DoubleHandles) ? 2 : 1);
}

void RangeSlider::SetRange(int aMinimum, int aMaximum)
{
	setMinimum(aMinimum);
	setMaximum(aMaximum);
}

float RangeSlider::currentPercentage() { return 0.0f; }
