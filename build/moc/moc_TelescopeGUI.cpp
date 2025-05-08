/****************************************************************************
** Meta object code from reading C++ file 'TelescopeGUI.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../TelescopeGUI.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'TelescopeGUI.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN12TelescopeGUIE_t {};
} // unnamed namespace

template <> constexpr inline auto TelescopeGUI::qt_create_metaobjectdata<qt_meta_tag_ZN12TelescopeGUIE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "TelescopeGUI",
        "startDiscovery",
        "",
        "stopDiscovery",
        "processPendingDatagrams",
        "connectToSelectedTelescope",
        "onWebSocketConnected",
        "onWebSocketDisconnected",
        "onTextMessageReceived",
        "message",
        "updateMountDisplay",
        "updateCameraDisplay",
        "updateFocuserDisplay",
        "updateEnvironmentDisplay",
        "updateImageDisplay",
        "updateDiskDisplay",
        "updateDewHeaterDisplay",
        "updateOrientationDisplay",
        "updateTimeDisplay",
        "startAutomaticDownload",
        "stopAutomaticDownload",
        "updateDownloadProgress",
        "currentFile",
        "filesCompleted",
        "totalFiles",
        "bytesReceived",
        "bytesTotal",
        "onDirectoryDownloadStarted",
        "directory",
        "onFileDownloadStarted",
        "fileName",
        "onFileDownloaded",
        "success",
        "onDirectoryDownloaded",
        "onAllDownloadsComplete",
        "startSlewAndImage",
        "cancelSlewAndImage",
        "slewAndImageTimerTimeout",
        "updateSlewAndImageStatus",
        "initializeTelescope",
        "startTelescopeAlignment",
        "checkMountStatus"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'startDiscovery'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'stopDiscovery'
        QtMocHelpers::SlotData<void()>(3, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'processPendingDatagrams'
        QtMocHelpers::SlotData<void()>(4, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'connectToSelectedTelescope'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onWebSocketConnected'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onWebSocketDisconnected'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onTextMessageReceived'
        QtMocHelpers::SlotData<void(const QString &)>(8, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 9 },
        }}),
        // Slot 'updateMountDisplay'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateCameraDisplay'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateFocuserDisplay'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateEnvironmentDisplay'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateImageDisplay'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateDiskDisplay'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateDewHeaterDisplay'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateOrientationDisplay'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateTimeDisplay'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'startAutomaticDownload'
        QtMocHelpers::SlotData<void()>(19, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'stopAutomaticDownload'
        QtMocHelpers::SlotData<void()>(20, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateDownloadProgress'
        QtMocHelpers::SlotData<void(const QString &, int, int, qint64, qint64)>(21, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 22 }, { QMetaType::Int, 23 }, { QMetaType::Int, 24 }, { QMetaType::LongLong, 25 },
            { QMetaType::LongLong, 26 },
        }}),
        // Slot 'onDirectoryDownloadStarted'
        QtMocHelpers::SlotData<void(const QString &)>(27, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 28 },
        }}),
        // Slot 'onFileDownloadStarted'
        QtMocHelpers::SlotData<void(const QString &)>(29, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 30 },
        }}),
        // Slot 'onFileDownloaded'
        QtMocHelpers::SlotData<void(const QString &, bool)>(31, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 30 }, { QMetaType::Bool, 32 },
        }}),
        // Slot 'onDirectoryDownloaded'
        QtMocHelpers::SlotData<void(const QString &)>(33, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 28 },
        }}),
        // Slot 'onAllDownloadsComplete'
        QtMocHelpers::SlotData<void()>(34, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'startSlewAndImage'
        QtMocHelpers::SlotData<void()>(35, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'cancelSlewAndImage'
        QtMocHelpers::SlotData<void()>(36, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'slewAndImageTimerTimeout'
        QtMocHelpers::SlotData<void()>(37, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateSlewAndImageStatus'
        QtMocHelpers::SlotData<void()>(38, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'initializeTelescope'
        QtMocHelpers::SlotData<void()>(39, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'startTelescopeAlignment'
        QtMocHelpers::SlotData<void()>(40, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'checkMountStatus'
        QtMocHelpers::SlotData<void()>(41, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<TelescopeGUI, qt_meta_tag_ZN12TelescopeGUIE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject TelescopeGUI::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12TelescopeGUIE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12TelescopeGUIE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12TelescopeGUIE_t>.metaTypes,
    nullptr
} };

void TelescopeGUI::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<TelescopeGUI *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->startDiscovery(); break;
        case 1: _t->stopDiscovery(); break;
        case 2: _t->processPendingDatagrams(); break;
        case 3: _t->connectToSelectedTelescope(); break;
        case 4: _t->onWebSocketConnected(); break;
        case 5: _t->onWebSocketDisconnected(); break;
        case 6: _t->onTextMessageReceived((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 7: _t->updateMountDisplay(); break;
        case 8: _t->updateCameraDisplay(); break;
        case 9: _t->updateFocuserDisplay(); break;
        case 10: _t->updateEnvironmentDisplay(); break;
        case 11: _t->updateImageDisplay(); break;
        case 12: _t->updateDiskDisplay(); break;
        case 13: _t->updateDewHeaterDisplay(); break;
        case 14: _t->updateOrientationDisplay(); break;
        case 15: _t->updateTimeDisplay(); break;
        case 16: _t->startAutomaticDownload(); break;
        case 17: _t->stopAutomaticDownload(); break;
        case 18: _t->updateDownloadProgress((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[5]))); break;
        case 19: _t->onDirectoryDownloadStarted((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 20: _t->onFileDownloadStarted((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 21: _t->onFileDownloaded((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
        case 22: _t->onDirectoryDownloaded((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 23: _t->onAllDownloadsComplete(); break;
        case 24: _t->startSlewAndImage(); break;
        case 25: _t->cancelSlewAndImage(); break;
        case 26: _t->slewAndImageTimerTimeout(); break;
        case 27: _t->updateSlewAndImageStatus(); break;
        case 28: _t->initializeTelescope(); break;
        case 29: _t->startTelescopeAlignment(); break;
        case 30: _t->checkMountStatus(); break;
        default: ;
        }
    }
}

const QMetaObject *TelescopeGUI::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *TelescopeGUI::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12TelescopeGUIE_t>.strings))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int TelescopeGUI::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 31)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 31;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 31)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 31;
    }
    return _id;
}
QT_WARNING_POP
