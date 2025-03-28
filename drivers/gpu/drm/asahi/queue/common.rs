// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Common queue functionality.
//!
//! Shared helpers used by the submission logic for multiple command types.

use crate::file;
use crate::fw::job::UserTimestamp;
use crate::fw::microseq;
use crate::fw::types::*;

use kernel::prelude::*;
use kernel::uaccess::{UserPtr, UserSlice};
use kernel::uapi;
use kernel::xarray;

use core::mem::MaybeUninit;

pub(super) fn build_attachments(pointer: u64, count: u32) -> Result<microseq::Attachments> {
    if count as usize > microseq::MAX_ATTACHMENTS {
        return Err(EINVAL);
    }

    const STRIDE: usize = core::mem::size_of::<uapi::drm_asahi_attachment>();
    let size = STRIDE * count as usize;

    // SAFETY: We only read this once, so there are no TOCTOU issues.
    let mut reader = UserSlice::new(pointer as UserPtr, size).reader();

    let mut attachments: microseq::Attachments = Default::default();

    for i in 0..count {
        let mut att: MaybeUninit<uapi::drm_asahi_attachment> = MaybeUninit::uninit();

        // SAFETY: The size of `att` is STRIDE
        reader.read_raw(unsafe {
            core::slice::from_raw_parts_mut(att.as_mut_ptr() as *mut MaybeUninit<u8>, STRIDE)
        })?;

        // SAFETY: All bit patterns in the struct are valid
        let att = unsafe { att.assume_init() };

        if att.flags != 0 {
            return Err(EINVAL);
        }
        if att.order < 1 || att.order > 6 {
            return Err(EINVAL);
        }

        let cache_lines = (att.size + 127) >> 7;
        attachments.list[i as usize] = microseq::Attachment {
            address: U64(att.pointer),
            size: cache_lines.try_into()?,
            unk_c: 0x17,
            unk_e: att.order as u16,
        };

        attachments.count += 1;
    }

    Ok(attachments)
}

pub(super) fn get_timestamp_object(
    objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    handle: u32,
    offset: u32,
) -> Result<Option<UserTimestamp>> {
    if handle == 0 {
        return Ok(None);
    }

    let object = objects.get(handle.try_into()?).ok_or(ENOENT)?;

    #[allow(irrefutable_let_patterns)]
    if let file::Object::TimestampBuffer(mapping) = object.borrow() {
        if (offset.checked_add(8).ok_or(EINVAL)?) as usize > mapping.size() {
            return Err(ERANGE);
        }
        Ok(Some(UserTimestamp {
            mapping: mapping.clone(),
            offset: offset as usize,
        }))
    } else {
        Err(EINVAL)
    }
}
