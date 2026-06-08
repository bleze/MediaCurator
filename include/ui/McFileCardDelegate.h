#pragma once
#include "ui/McCardDelegate.h"

namespace Mc {

class McFileCardDelegate : public McCardDelegate
{
	Q_OBJECT
public:
	explicit McFileCardDelegate(QObject* parent = nullptr)
		: McCardDelegate(Mode::Library, parent) {}
};

} // namespace Mc
