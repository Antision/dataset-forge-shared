#ifndef FONTS_H
#define FONTS_H

#include <QString>

/**
 * @brief Global font family: the embedded Ubuntu fonts (Light/Regular/Bold).
 *
 * Registered on first call. Two programs embed the same fonts under different
 * resource prefixes (tqe: :/res/fonts/, gallery_info: :/fonts/), so both are
 * tried; the first group that loads wins. Returns "Segoe UI" when the resources
 * are missing (e.g. a unit test without a resource file), so callers always get
 * a usable family. Must be called after the QApplication instance exists.
 *
 * The Light weight is applied by each program's main() when it sets the
 * application font; this function only guarantees the family is registered.
 */
const QString& appFontFamily();

#endif // FONTS_H
