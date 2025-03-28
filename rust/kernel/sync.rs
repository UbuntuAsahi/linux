// SPDX-License-Identifier: GPL-2.0

//! Synchronisation primitives.
//!
//! This module contains the kernel APIs related to synchronisation that have been ported or
//! wrapped for usage by Rust code in the kernel.

mod arc;
mod condvar;
pub mod lock;
mod locked_by;
pub mod poll;
pub mod rcu;

#[cfg(CONFIG_LOCKDEP)]
mod lockdep;
#[cfg(not(CONFIG_LOCKDEP))]
mod no_lockdep;
#[cfg(not(CONFIG_LOCKDEP))]
use no_lockdep as lockdep;

pub use arc::{Arc, ArcBorrow, UniqueArc};
pub use condvar::{new_condvar, CondVar, CondVarTimeoutResult};
pub use lock::global::{global_lock, GlobalGuard, GlobalLock, GlobalLockBackend, GlobalLockedBy};
pub use lock::mutex::{new_mutex, Mutex};
pub use lock::spinlock::{new_spinlock, SpinLock};
pub use lockdep::{LockClassKey, StaticLockClassKey};
pub use locked_by::LockedBy;

impl Default for StaticLockClassKey {
    fn default() -> Self {
        Self::new()
    }
}

/// Defines a new static lock class and returns a pointer to it.
#[doc(hidden)]
#[macro_export]
macro_rules! static_lock_class {
    () => {{
        static CLASS: $crate::sync::StaticLockClassKey = $crate::sync::StaticLockClassKey::new();
        CLASS.key()
    }};
}

/// Returns the given string, if one is provided, otherwise generates one based on the source code
/// location.
#[doc(hidden)]
#[macro_export]
macro_rules! optional_name {
    () => {
        $crate::c_str!(::core::concat!(::core::file!(), ":", ::core::line!()))
    };
    ($name:literal) => {
        $crate::c_str!($name)
    };
}
