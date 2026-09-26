#pragma once

/// \file python_ref.hpp
/// \brief Provides private move-only ownership for strong CPython references.

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

namespace mt5bridge::runtime {

/// \class PyRef
/// \brief Owns one strong CPython reference inside the runtime DLL.
class PyRef {
public:
    /// \brief Takes ownership of a new Python reference.
    /// \param object Owned Python object; nullptr is permitted.
    explicit PyRef(PyObject *object = nullptr) : object_(object) {}

    /// \brief Releases the owned Python reference.
    ~PyRef() { Py_XDECREF(object_); }

    /// \brief Strong Python references are not copyable.
    PyRef(const PyRef &) = delete;

    /// \brief Strong Python references are not copy-assignable.
    PyRef &operator=(const PyRef &) = delete;

    /// \brief Transfers ownership from another reference wrapper.
    /// \param other Wrapper whose reference is transferred.
    PyRef(PyRef &&other) noexcept : object_(other.release()) {}

    /// \brief Replaces the owned reference by moving from another wrapper.
    /// \param other Wrapper whose reference is transferred.
    /// \return Reference to this wrapper.
    PyRef &operator=(PyRef &&other) noexcept {
        if (this != &other) {
            Py_XDECREF(object_);
            object_ = other.release();
        }
        return *this;
    }

    /// \brief Returns the borrowed Python object pointer.
    /// \return Borrowed pointer, or nullptr when empty.
    PyObject *get() const { return object_; }

    /// \brief Relinquishes ownership without decrementing the reference count.
    /// \return Previously owned new reference, or nullptr.
    PyObject *release() {
        PyObject *result = object_;
        object_ = nullptr;
        return result;
    }

    /// \brief Tests whether this wrapper owns a reference.
    /// \return True when the stored pointer is non-null.
    explicit operator bool() const { return object_ != nullptr; }

private:
    PyObject *object_ = nullptr;
};

} // namespace mt5bridge::runtime
