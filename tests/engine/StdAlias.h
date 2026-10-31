// A host that names the standard library "mystd" and "other_std". The module attribute "[std(\"mystd\")];" uses them.
#pragma once

#include <string>
#include <vector>

namespace mystd = std;
namespace other_std = std;
