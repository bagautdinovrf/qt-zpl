#pragma once

#include <QtCore/qglobal.h>

#if defined(QTZPL_STATIC)
#  define QTZPL_EXPORT
#elif defined(QTZPL_BUILDING_LIBRARY)
#  define QTZPL_EXPORT Q_DECL_EXPORT
#else
#  define QTZPL_EXPORT Q_DECL_IMPORT
#endif
