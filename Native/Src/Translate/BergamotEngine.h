#pragma once
#include "TranslateTypes.h"

namespace BergamotEngine {
	bool isAvailable();
	TranslateResult translateSync(const TranslateRequest& req);
}
