#pragma once
#include "ui/McCardDelegate.h"

namespace Mc {

class McJobCardDelegate : public McCardDelegate
{
	Q_OBJECT
public:
	explicit McJobCardDelegate(QObject* parent = nullptr)
		: McCardDelegate(Mode::JobQueue, parent) {}
};

} // namespace Mc
