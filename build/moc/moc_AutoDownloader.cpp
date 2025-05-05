/****************************************************************************
** Meta object code from reading C++ file 'AutoDownloader.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "AutoDownloader.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'AutoDownloader.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN14AutoDownloaderE_t {};
} // unnamed namespace

template <> constexpr inline auto AutoDownloader::qt_create_metaobjectdata<qt_meta_tag_ZN14AutoDownloaderE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "AutoDownloader",
        "directoryDownloadStarted",
        "",
        "directory",
        "fileDownloadStarted",
        "fileName",
        "fileDownloaded",
        "success",
        "directoryDownloaded",
        "allDownloadsComplete",
        "downloadProgress",
        "currentFile",
        "filesCompleted",
        "totalFiles",
        "bytesReceived",
        "bytesTotal",
        "processDirectoryList",
        "message",
        "onFileDownloaded",
        "onDownloadProgress",
        "onTextMessageReceived"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'directoryDownloadStarted'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'fileDownloadStarted'
        QtMocHelpers::SignalData<void(const QString &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 5 },
        }}),
        // Signal 'fileDownloaded'
        QtMocHelpers::SignalData<void(const QString &, bool)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 5 }, { QMetaType::Bool, 7 },
        }}),
        // Signal 'directoryDownloaded'
        QtMocHelpers::SignalData<void(const QString &)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'allDownloadsComplete'
        QtMocHelpers::SignalData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'downloadProgress'
        QtMocHelpers::SignalData<void(const QString &, int, int, qint64, qint64)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 11 }, { QMetaType::Int, 12 }, { QMetaType::Int, 13 }, { QMetaType::LongLong, 14 },
            { QMetaType::LongLong, 15 },
        }}),
        // Slot 'processDirectoryList'
        QtMocHelpers::SlotData<void(const QString &)>(16, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 17 },
        }}),
        // Slot 'onFileDownloaded'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onDownloadProgress'
        QtMocHelpers::SlotData<void(qint64, qint64)>(19, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::LongLong, 14 }, { QMetaType::LongLong, 15 },
        }}),
        // Slot 'onTextMessageReceived'
        QtMocHelpers::SlotData<void(const QString &)>(20, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 17 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AutoDownloader, qt_meta_tag_ZN14AutoDownloaderE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject AutoDownloader::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14AutoDownloaderE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14AutoDownloaderE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14AutoDownloaderE_t>.metaTypes,
    nullptr
} };

void AutoDownloader::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AutoDownloader *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->directoryDownloadStarted((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->fileDownloadStarted((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->fileDownloaded((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
        case 3: _t->directoryDownloaded((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 4: _t->allDownloadsComplete(); break;
        case 5: _t->downloadProgress((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[5]))); break;
        case 6: _t->processDirectoryList((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 7: _t->onFileDownloaded(); break;
        case 8: _t->onDownloadProgress((*reinterpret_cast< std::add_pointer_t<qint64>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2]))); break;
        case 9: _t->onTextMessageReceived((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (AutoDownloader::*)(const QString & )>(_a, &AutoDownloader::directoryDownloadStarted, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (AutoDownloader::*)(const QString & )>(_a, &AutoDownloader::fileDownloadStarted, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (AutoDownloader::*)(const QString & , bool )>(_a, &AutoDownloader::fileDownloaded, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (AutoDownloader::*)(const QString & )>(_a, &AutoDownloader::directoryDownloaded, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (AutoDownloader::*)()>(_a, &AutoDownloader::allDownloadsComplete, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (AutoDownloader::*)(const QString & , int , int , qint64 , qint64 )>(_a, &AutoDownloader::downloadProgress, 5))
            return;
    }
}

const QMetaObject *AutoDownloader::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AutoDownloader::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14AutoDownloaderE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int AutoDownloader::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 10)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 10;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 10)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 10;
    }
    return _id;
}

// SIGNAL 0
void AutoDownloader::directoryDownloadStarted(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void AutoDownloader::fileDownloadStarted(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void AutoDownloader::fileDownloaded(const QString & _t1, bool _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2);
}

// SIGNAL 3
void AutoDownloader::directoryDownloaded(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void AutoDownloader::allDownloadsComplete()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void AutoDownloader::downloadProgress(const QString & _t1, int _t2, int _t3, qint64 _t4, qint64 _t5)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1, _t2, _t3, _t4, _t5);
}
QT_WARNING_POP
