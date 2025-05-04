/****************************************************************************
** Meta object code from reading C++ file 'TelescopeDataProcessor.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../TelescopeDataProcessor.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'TelescopeDataProcessor.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN22TelescopeDataProcessorE_t {};
} // unnamed namespace

template <> constexpr inline auto TelescopeDataProcessor::qt_create_metaobjectdata<qt_meta_tag_ZN22TelescopeDataProcessorE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "TelescopeDataProcessor",
        "mountStatusUpdated",
        "",
        "cameraStatusUpdated",
        "focuserStatusUpdated",
        "environmentStatusUpdated",
        "newImageAvailable",
        "diskStatusUpdated",
        "dewHeaterStatusUpdated",
        "orientationStatusUpdated"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'mountStatusUpdated'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'cameraStatusUpdated'
        QtMocHelpers::SignalData<void()>(3, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'focuserStatusUpdated'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'environmentStatusUpdated'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'newImageAvailable'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'diskStatusUpdated'
        QtMocHelpers::SignalData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'dewHeaterStatusUpdated'
        QtMocHelpers::SignalData<void()>(8, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'orientationStatusUpdated'
        QtMocHelpers::SignalData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<TelescopeDataProcessor, qt_meta_tag_ZN22TelescopeDataProcessorE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject TelescopeDataProcessor::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN22TelescopeDataProcessorE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN22TelescopeDataProcessorE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN22TelescopeDataProcessorE_t>.metaTypes,
    nullptr
} };

void TelescopeDataProcessor::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<TelescopeDataProcessor *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->mountStatusUpdated(); break;
        case 1: _t->cameraStatusUpdated(); break;
        case 2: _t->focuserStatusUpdated(); break;
        case 3: _t->environmentStatusUpdated(); break;
        case 4: _t->newImageAvailable(); break;
        case 5: _t->diskStatusUpdated(); break;
        case 6: _t->dewHeaterStatusUpdated(); break;
        case 7: _t->orientationStatusUpdated(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::mountStatusUpdated, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::cameraStatusUpdated, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::focuserStatusUpdated, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::environmentStatusUpdated, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::newImageAvailable, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::diskStatusUpdated, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::dewHeaterStatusUpdated, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (TelescopeDataProcessor::*)()>(_a, &TelescopeDataProcessor::orientationStatusUpdated, 7))
            return;
    }
}

const QMetaObject *TelescopeDataProcessor::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *TelescopeDataProcessor::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN22TelescopeDataProcessorE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int TelescopeDataProcessor::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 8)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 8;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 8)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 8;
    }
    return _id;
}

// SIGNAL 0
void TelescopeDataProcessor::mountStatusUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void TelescopeDataProcessor::cameraStatusUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void TelescopeDataProcessor::focuserStatusUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void TelescopeDataProcessor::environmentStatusUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void TelescopeDataProcessor::newImageAvailable()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void TelescopeDataProcessor::diskStatusUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void TelescopeDataProcessor::dewHeaterStatusUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void TelescopeDataProcessor::orientationStatusUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}
QT_WARNING_POP
