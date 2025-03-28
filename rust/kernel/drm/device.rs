// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM device.
//!
//! C header: [`include/linux/drm/drm_device.h`](srctree/include/linux/drm/drm_device.h)

use crate::{
    bindings, device, drm,
    drm::drv::AllocImpl,
    error::code::*,
    error::from_err_ptr,
    error::Result,
    ffi,
    types::{ARef, AlwaysRefCounted, ForeignOwnable, Opaque},
};
use core::{marker::PhantomData, ops::Deref, ptr::NonNull};

#[cfg(CONFIG_DRM_LEGACY)]
macro_rules! drm_legacy_fields {
    ( $($field:ident: $val:expr),* $(,)? ) => {
        bindings::drm_driver {
            $( $field: $val ),*,
            firstopen: None,
            preclose: None,
            dma_ioctl: None,
            dma_quiescent: None,
            context_dtor: None,
            irq_handler: None,
            irq_preinstall: None,
            irq_postinstall: None,
            irq_uninstall: None,
            get_vblank_counter: None,
            enable_vblank: None,
            disable_vblank: None,
            dev_priv_size: 0,
        }
    }
}

#[cfg(not(CONFIG_DRM_LEGACY))]
macro_rules! drm_legacy_fields {
    ( $($field:ident: $val:expr),* $(,)? ) => {
        bindings::drm_driver {
            $( $field: $val ),*
        }
    }
}

/// A typed DRM device with a specific `drm::drv::Driver` implementation. The device is always
/// reference-counted.
///
/// # Invariants
///
/// `drm_dev_release()` can be called from any non-atomic context.
#[repr(transparent)]
pub struct Device<T: drm::drv::Driver>(Opaque<bindings::drm_device>, PhantomData<T>);

impl<T: drm::drv::Driver> Device<T> {
    const VTABLE: bindings::drm_driver = drm_legacy_fields! {
        load: None,
        open: Some(drm::file::open_callback::<T::File>),
        postclose: Some(drm::file::postclose_callback::<T::File>),
        unload: None,
        release: Some(Self::release),
        master_set: None,
        master_drop: None,
        debugfs_init: None,
        gem_create_object: T::Object::ALLOC_OPS.gem_create_object,
        prime_handle_to_fd: T::Object::ALLOC_OPS.prime_handle_to_fd,
        prime_fd_to_handle: T::Object::ALLOC_OPS.prime_fd_to_handle,
        gem_prime_import: T::Object::ALLOC_OPS.gem_prime_import,
        gem_prime_import_sg_table: T::Object::ALLOC_OPS.gem_prime_import_sg_table,
        dumb_create: T::Object::ALLOC_OPS.dumb_create,
        dumb_map_offset: T::Object::ALLOC_OPS.dumb_map_offset,
        show_fdinfo: None,
        fbdev_probe: None,

        major: T::INFO.major,
        minor: T::INFO.minor,
        patchlevel: T::INFO.patchlevel,
        name: T::INFO.name.as_char_ptr() as *mut _,
        desc: T::INFO.desc.as_char_ptr() as *mut _,
        date: T::INFO.date.as_char_ptr() as *mut _,

        driver_features: T::FEATURES,
        ioctls: T::IOCTLS.as_ptr(),
        num_ioctls: T::IOCTLS.len() as i32,
        fops: &Self::GEM_FOPS as _,
    };

    const GEM_FOPS: bindings::file_operations = drm::gem::create_fops();

    /// Create a new `drm::device::Device` for a `drm::drv::Driver`.
    pub fn new(dev: &device::Device) -> Result<ARef<Self>> {
        // SAFETY: `dev` is valid by its type invarants; `VTABLE`, as a `const` is pinned to the
        // read-only section of the compilation.
        let raw_drm = unsafe { bindings::drm_dev_alloc(&Self::VTABLE, dev.as_raw()) };
        let raw_drm = NonNull::new(from_err_ptr(raw_drm)? as *mut _).ok_or(ENOMEM)?;

        // SAFETY: The reference count is one, and now we take ownership of that reference as a
        // drm::device::Device.
        Ok(unsafe { ARef::<Self>::from_raw(raw_drm) })
    }

    pub(crate) fn as_raw(&self) -> *mut bindings::drm_device {
        self.0.get()
    }

    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count,
    /// i.e. it must be ensured that the reference count of the C `struct drm_device` `ptr` points
    /// to can't drop to zero, for the duration of this function call and the entire duration when
    /// the returned reference exists.
    pub(crate) unsafe fn borrow<'a>(ptr: *const bindings::drm_device) -> &'a Self {
        // SAFETY: Safe by the safety requirements of this function.
        unsafe { &*ptr.cast() }
    }

    fn raw_data(&self) -> *mut ffi::c_void {
        // SAFETY: `self` is guaranteed to hold a valid `bindings::drm_device` pointer.
        unsafe { *self.as_raw() }.dev_private
    }

    /// # Safety
    ///
    /// Must be called only once after device creation.
    pub(crate) unsafe fn set_raw_data(&self, ptr: *const ffi::c_void) {
        // SAFETY: Safe as by the safety precondition.
        unsafe { &mut *self.as_raw() }.dev_private = ptr as _;
    }

    #[allow(clippy::missing_safety_doc)]
    unsafe extern "C" fn release(drm: *mut bindings::drm_device) {
        // SAFETY: Guaranteed to be a valid pointer to a `struct drm_device`.
        let drm = unsafe { Self::borrow(drm) };

        if !drm.raw_data().is_null() {
            // SAFETY: `Self::data` is either `NULL` or a valid `ForeignOwnable`.
            unsafe { <T::Data as ForeignOwnable>::from_foreign(drm.raw_data()) };
        }
    }
}

/// Same as [`Device`], but with an accessor of the device' driver private data.
#[repr(transparent)]
pub struct RegisteredDevice<T: drm::drv::Driver>(Device<T>);

impl<T: drm::drv::Driver> RegisteredDevice<T> {
    /// Not intended to be called externally, except via declare_drm_ioctls!()
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count,
    /// i.e. it must be ensured that the reference count of the C `struct drm_device` `ptr` points
    /// to can't drop to zero, for the duration of this function call and the entire duration when
    /// the returned reference exists.
    ///
    /// Additionally, callers must ensure that the corresponding `struct drm_device` is registered.
    #[doc(hidden)]
    pub unsafe fn borrow<'a>(ptr: *const bindings::drm_device) -> &'a Self {
        // SAFETY: By the safety requirements of this function `ptr` is valid.
        unsafe { &*ptr.cast() }
    }

    /// Returns a borrowed reference to the user data associated with this Device.
    pub fn data(&self) -> <T::Data as ForeignOwnable>::Borrowed<'_> {
        // SAFETY: `dev_private` is always set once the device is registered.
        unsafe { T::Data::borrow(self.raw_data()) }
    }
}

impl<T: drm::drv::Driver> Deref for RegisteredDevice<T> {
    type Target = Device<T>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

// SAFETY: DRM device objects are always reference counted and the get/put functions
// satisfy the requirements.
unsafe impl<T: drm::drv::Driver> AlwaysRefCounted for Device<T> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::drm_dev_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::drm_dev_put(obj.cast().as_ptr()) };
    }
}

impl<T: drm::drv::Driver> AsRef<device::Device> for Device<T> {
    fn as_ref(&self) -> &device::Device {
        // SAFETY: `bindings::drm_device::dev` is valid as long as the DRM device itself is valid,
        // which is guaranteed by the type invariant.
        unsafe { device::Device::as_ref((*self.as_raw()).dev) }
    }
}

// SAFETY: As by the type invariant `Device` can be sent to any thread.
unsafe impl<T: drm::drv::Driver> Send for Device<T> {}

// SAFETY: `Device` can be shared among threads because all immutable methods are protected by the
// synchronization in `struct drm_device`.
unsafe impl<T: drm::drv::Driver> Sync for Device<T> {}
