// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(clippy::unusual_byte_groupings)]

//! Compute work queue.
//!
//! A compute queue consists of one underlying WorkQueue.
//! This module is in charge of creating all of the firmware structures required to submit compute
//! work to the GPU, based on the userspace command buffer.

use super::common;
use crate::alloc::Allocator;
use crate::debug::*;
use crate::driver::AsahiDriver;
use crate::fw::types::*;
use crate::gpu::GpuManager;
use crate::{file, fw, gpu, microseq};
use crate::{inner_ptr, inner_weak_ptr};
use core::mem::MaybeUninit;
use core::sync::atomic::Ordering;
use kernel::dma_fence::RawDmaFence;
use kernel::drm::sched::Job;
use kernel::prelude::*;
use kernel::sync::Arc;
use kernel::types::ForeignOwnable;
use kernel::uaccess::{UserPtr, UserSlice};
use kernel::uapi;
use kernel::xarray;

const DEBUG_CLASS: DebugFlags = DebugFlags::Compute;

#[versions(AGX)]
impl super::QueueInner::ver {
    /// Submit work to a compute queue.
    pub(super) fn submit_compute(
        &self,
        job: &mut Job<super::QueueJob::ver>,
        cmd: &uapi::drm_asahi_command,
        result_writer: Option<super::ResultWriter>,
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        id: u64,
        flush_stamps: bool,
    ) -> Result {
        if cmd.cmd_type != uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE {
            return Err(EINVAL);
        }

        let data = unsafe { &<KBox<AsahiDriver>>::borrow(self.dev.as_ref().get_drvdata()).data };
        let gpu = match data.gpu.as_any().downcast_ref::<gpu::GpuManager::ver>() {
            Some(gpu) => gpu,
            None => {
                dev_crit!(self.dev.as_ref(), "GpuManager mismatched with Queue!\n");
                return Err(EIO);
            }
        };

        let mut alloc = gpu.alloc();
        let kalloc = &mut *alloc;

        mod_dev_dbg!(self.dev, "[Submission {}] Compute!\n", id);

        let cmdbuf_read_size =
            (cmd.cmd_buffer_size as usize).min(core::mem::size_of::<uapi::drm_asahi_cmd_compute>());
        // SAFETY: This is the sole UserSlice instance for this cmd_buffer.
        let mut cmdbuf_reader =
            UserSlice::new(cmd.cmd_buffer as UserPtr, cmd.cmd_buffer_size as usize).reader();

        let mut cmdbuf: uapi::drm_asahi_cmd_compute = Default::default();
        // SAFETY: The output pointer is valid, and the size does not exceed the type size
        // per the min() above, and all bit patterns are valid.
        // cmdbuf_reader.read_raw(&mut cmdbuf as *mut _ as *mut MaybeUninit<u8>, cmdbuf_read_size)})?;
        cmdbuf_reader.read_raw(unsafe {
            core::slice::from_raw_parts_mut(
                &mut cmdbuf as *mut _ as *mut MaybeUninit<u8>,
                cmdbuf_read_size,
            )
        })?;

        if cmdbuf.flags & !(uapi::ASAHI_COMPUTE_NO_PREEMPTION as u64) != 0 {
            return Err(EINVAL);
        }

        let mut user_timestamps: fw::job::UserTimestamps = Default::default();

        let mut ext_ptr = cmdbuf.extensions;
        while ext_ptr != 0 {
            // SAFETY: There is a double read from userspace here, but there is no TOCTOU
            // issue since at worst the extension parse below will read garbage, and
            // we do not trust any fields anyway.
            let ext_type = UserSlice::new(ext_ptr as UserPtr, 4)
                .reader()
                .read::<u32>()?;

            match ext_type {
                uapi::ASAHI_COMPUTE_EXT_TIMESTAMPS => {
                    let mut ext_user_timestamps: uapi::drm_asahi_cmd_compute_user_timestamps =
                        Default::default();

                    // SAFETY: See above
                    let mut ext_reader = UserSlice::new(
                        ext_ptr as UserPtr,
                        core::mem::size_of::<uapi::drm_asahi_cmd_compute_user_timestamps>(),
                    )
                    .reader();
                    // SAFETY: The output buffer is valid and of the correct size, and all bit
                    // patterns are valid.
                    ext_reader.read_raw(unsafe {
                        core::slice::from_raw_parts_mut(
                            &mut ext_user_timestamps as *mut _ as *mut MaybeUninit<u8>,
                            core::mem::size_of::<uapi::drm_asahi_cmd_compute_user_timestamps>(),
                        )
                    })?;

                    user_timestamps.start = common::get_timestamp_object(
                        objects,
                        ext_user_timestamps.start_handle,
                        ext_user_timestamps.start_offset,
                    )?;
                    user_timestamps.end = common::get_timestamp_object(
                        objects,
                        ext_user_timestamps.end_handle,
                        ext_user_timestamps.end_offset,
                    )?;

                    ext_ptr = ext_user_timestamps.next;
                }
                _ => {
                    cls_pr_debug!(Errors, "Unknown extension {}\n", ext_type);
                    return Err(EINVAL);
                }
            }
        }

        // This sequence number increases per new client/VM? assigned to some slot,
        // but it's unclear *which* slot...
        let slot_client_seq: u8 = (self.id & 0xff) as u8;

        let vm_bind = job.vm_bind.clone();

        mod_dev_dbg!(
            self.dev,
            "[Submission {}] VM slot = {}\n",
            id,
            vm_bind.slot()
        );

        let notifier = self.notifier.clone();

        let fence = job.fence.clone();
        let comp_job = job.get_comp()?;
        let ev_comp = comp_job.event_info();

        let preempt2_off = gpu.get_cfg().compute_preempt1_size;
        let preempt3_off = preempt2_off + 8;
        let preempt4_off = preempt3_off + 8;
        let preempt5_off = preempt4_off + 8;
        let preempt_size = preempt5_off + 8;

        let preempt_buf = self
            .ualloc
            .lock()
            .array_empty_tagged(preempt_size, b"CPMT")?;

        mod_dev_dbg!(
            self.dev,
            "[Submission {}] Event #{} {:#x?} -> {:#x?}\n",
            id,
            ev_comp.slot,
            ev_comp.value,
            ev_comp.value.next(),
        );

        let timestamps = Arc::new(
            kalloc.shared.new_default::<fw::job::JobTimestamps>()?,
            GFP_KERNEL,
        )?;

        let uuid = cmdbuf.cmd_id;

        mod_dev_dbg!(self.dev, "[Submission {}] UUID = {:#x?}\n", id, uuid);

        // TODO: check
        #[ver(V >= V13_0B4)]
        let count = self.counter.fetch_add(1, Ordering::Relaxed);

        let comp = GpuObject::new_init_prealloc(
            kalloc.gpu_ro.alloc_object()?,
            |ptr: GpuWeakPointer<fw::compute::RunCompute::ver>| {
                let has_result = result_writer.is_some();
                let notifier = notifier.clone();
                let vm_bind = vm_bind.clone();
                try_init!(fw::compute::RunCompute::ver {
                    preempt_buf: preempt_buf,
                    micro_seq: {
                        let mut builder = microseq::Builder::new();

                        let stats = gpu.initdata.runtime_pointers.stats.comp.weak_pointer();

                        let start_comp = builder.add(microseq::StartCompute::ver {
                            header: microseq::op::StartCompute::HEADER,
                            unk_pointer: inner_weak_ptr!(ptr, unk_pointee),
                            #[ver(G < G14X)]
                            job_params1: Some(inner_weak_ptr!(ptr, job_params1)),
                            #[ver(G >= G14X)]
                            job_params1: None,
                            #[ver(G >= G14X)]
                            registers: inner_weak_ptr!(ptr, registers),
                            stats,
                            work_queue: ev_comp.info_ptr,
                            vm_slot: vm_bind.slot(),
                            unk_28: 0x1,
                            event_generation: self.id as u32,
                            event_seq: U64(ev_comp.event_seq),
                            unk_38: 0x0,
                            job_params2: inner_weak_ptr!(ptr, job_params2),
                            unk_44: 0x0,
                            uuid,
                            attachments: common::build_attachments(
                                cmdbuf.attachments,
                                cmdbuf.attachment_count,
                            )?,
                            padding: Default::default(),
                            #[ver(V >= V13_0B4)]
                            unk_flag: inner_weak_ptr!(ptr, unk_flag),
                            #[ver(V >= V13_0B4)]
                            counter: U64(count),
                            #[ver(V >= V13_0B4)]
                            notifier_buf: inner_weak_ptr!(notifier.weak_pointer(), state.unk_buf),
                        })?;

                        if has_result || user_timestamps.any() {
                            builder.add(microseq::Timestamp::ver {
                                header: microseq::op::Timestamp::new(true),
                                command_time: inner_weak_ptr!(ptr, command_time),
                                ts_pointers: inner_weak_ptr!(ptr, timestamp_pointers),
                                update_ts: inner_weak_ptr!(ptr, timestamp_pointers.start_addr),
                                work_queue: ev_comp.info_ptr,
                                user_ts_pointers: inner_weak_ptr!(ptr, user_timestamp_pointers),
                                #[ver(V >= V13_0B4)]
                                unk_ts: inner_weak_ptr!(ptr, context_store_req),
                                uuid,
                                unk_30_padding: 0,
                            })?;
                        }

                        #[ver(G < G14X)]
                        builder.add(microseq::WaitForIdle {
                            header: microseq::op::WaitForIdle::new(microseq::Pipe::Compute),
                        })?;
                        #[ver(G >= G14X)]
                        builder.add(microseq::WaitForIdle2 {
                            header: microseq::op::WaitForIdle2::HEADER,
                        })?;

                        if has_result || user_timestamps.any() {
                            builder.add(microseq::Timestamp::ver {
                                header: microseq::op::Timestamp::new(false),
                                command_time: inner_weak_ptr!(ptr, command_time),
                                ts_pointers: inner_weak_ptr!(ptr, timestamp_pointers),
                                update_ts: inner_weak_ptr!(ptr, timestamp_pointers.end_addr),
                                work_queue: ev_comp.info_ptr,
                                user_ts_pointers: inner_weak_ptr!(ptr, user_timestamp_pointers),
                                #[ver(V >= V13_0B4)]
                                unk_ts: inner_weak_ptr!(ptr, context_store_req),
                                uuid,
                                unk_30_padding: 0,
                            })?;
                        }

                        let off = builder.offset_to(start_comp);
                        builder.add(microseq::FinalizeCompute::ver {
                            header: microseq::op::FinalizeCompute::HEADER,
                            stats,
                            work_queue: ev_comp.info_ptr,
                            vm_slot: vm_bind.slot(),
                            #[ver(V < V13_0B4)]
                            unk_18: 0,
                            job_params2: inner_weak_ptr!(ptr, job_params2),
                            unk_24: 0,
                            uuid,
                            fw_stamp: ev_comp.fw_stamp_pointer,
                            stamp_value: ev_comp.value.next(),
                            unk_38: 0,
                            unk_3c: 0,
                            unk_40: 0,
                            unk_44: 0,
                            unk_48: 0,
                            unk_4c: 0,
                            unk_50: 0,
                            unk_54: 0,
                            unk_58: 0,
                            #[ver(G == G14 && V < V13_0B4)]
                            unk_5c_g14: U64(0),
                            restart_branch_offset: off,
                            has_attachments: (cmdbuf.attachment_count > 0) as u32,
                            #[ver(V >= V13_0B4)]
                            unk_64: Default::default(),
                            #[ver(V >= V13_0B4)]
                            unk_flag: inner_weak_ptr!(ptr, unk_flag),
                            #[ver(V >= V13_0B4)]
                            unk_79: Default::default(),
                        })?;

                        builder.add(microseq::RetireStamp {
                            header: microseq::op::RetireStamp::HEADER,
                        })?;
                        builder.build(&mut kalloc.private)?
                    },
                    notifier,
                    vm_bind,
                    timestamps,
                    user_timestamps,
                })
            },
            |inner, _ptr| {
                let vm_slot = vm_bind.slot();
                try_init!(fw::compute::raw::RunCompute::ver {
                    tag: fw::workqueue::CommandType::RunCompute,
                    #[ver(V >= V13_0B4)]
                    counter: U64(count),
                    unk_4: 0,
                    vm_slot,
                    notifier: inner.notifier.gpu_pointer(),
                    unk_pointee: Default::default(),
                    #[ver(G < G14X)]
                    __pad0: Default::default(),
                    #[ver(G < G14X)]
                    job_params1 <- try_init!(fw::compute::raw::JobParameters1 {
                        preempt_buf1: inner.preempt_buf.gpu_pointer(),
                        encoder: U64(cmdbuf.encoder_ptr),
                        // buf2-5 Only if internal program is used
                        preempt_buf2: inner.preempt_buf.gpu_offset_pointer(preempt2_off),
                        preempt_buf3: inner.preempt_buf.gpu_offset_pointer(preempt3_off),
                        preempt_buf4: inner.preempt_buf.gpu_offset_pointer(preempt4_off),
                        preempt_buf5: inner.preempt_buf.gpu_offset_pointer(preempt5_off),
                        pipeline_base: U64(cmdbuf.usc_base),
                        unk_38: U64(0x8c60),
                        helper_program: cmdbuf.helper_program, // Internal program addr | 1
                        unk_44: 0,
                        helper_arg: U64(cmdbuf.helper_arg), // Only if internal program used
                        helper_cfg: cmdbuf.helper_cfg, // 0x40 if internal program used
                        unk_54: 0,
                        unk_58: 1,
                        unk_5c: 0,
                        iogpu_unk_40: cmdbuf.iogpu_unk_40, // 0x1c if internal program used
                        __pad: Default::default(),
                    }),
                    #[ver(G >= G14X)]
                    registers: fw::job::raw::RegisterArray::new(
                        inner_weak_ptr!(_ptr, registers.registers),
                        |r| {
                            r.add(0x1a510, inner.preempt_buf.gpu_pointer().into());
                            r.add(0x1a420, cmdbuf.encoder_ptr);
                            // buf2-5 Only if internal program is used
                            r.add(0x1a4d0, inner.preempt_buf.gpu_offset_pointer(preempt2_off).into());
                            r.add(0x1a4d8, inner.preempt_buf.gpu_offset_pointer(preempt3_off).into());
                            r.add(0x1a4e0, inner.preempt_buf.gpu_offset_pointer(preempt4_off).into());
                            r.add(0x1a4e8, inner.preempt_buf.gpu_offset_pointer(preempt5_off).into());
                            r.add(0x10071, cmdbuf.usc_base); // USC_EXEC_BASE_CP
                            r.add(0x11841, cmdbuf.helper_program.into());
                            r.add(0x11849, cmdbuf.helper_arg);
                            r.add(0x11f81, cmdbuf.helper_cfg.into());
                            r.add(0x1a440, 0x24201);
                            r.add(0x12091, cmdbuf.iogpu_unk_40.into());
                            /*
                            r.add(0x10201, 0x100); // Some kind of counter?? Does this matter?
                            r.add(0x10428, 0x100); // Some kind of counter?? Does this matter?
                            */
                        }
                    ),
                    __pad1: Default::default(),
                    microsequence: inner.micro_seq.gpu_pointer(),
                    microsequence_size: inner.micro_seq.len() as u32,
                    job_params2 <- try_init!(fw::compute::raw::JobParameters2::ver {
                        #[ver(V >= V13_0B4)]
                        unk_0_0: 0,
                        unk_0: Default::default(),
                        preempt_buf1: inner.preempt_buf.gpu_pointer(),
                        encoder_end: U64(cmdbuf.encoder_end),
                        unk_34: Default::default(),
                        #[ver(G < G14X)]
                        unk_g14x: 0,
                        #[ver(G >= G14X)]
                        unk_g14x: 0x24201,
                        unk_58: 0,
                        #[ver(V < V13_0B4)]
                        unk_5c: 0,
                    }),
                    encoder_params <- try_init!(fw::job::raw::EncoderParams {
                        unk_8: 0x0,     // fixed
                        sync_grow: 0x0, // check!
                        unk_10: 0x0,    // fixed
                        encoder_id: cmdbuf.encoder_id,
                        unk_18: 0x0, // fixed
                        unk_mask: cmdbuf.unk_mask,
                        sampler_array: U64(cmdbuf.sampler_array),
                        sampler_count: cmdbuf.sampler_count,
                        sampler_max: cmdbuf.sampler_max,
                    }),
                    meta <- try_init!(fw::job::raw::JobMeta {
                        unk_0: 0,
                        unk_2: 0,
                        // TODO: make separate flag
                        no_preemption: (cmdbuf.flags
                        & uapi::ASAHI_COMPUTE_NO_PREEMPTION as u64
                        != 0) as u8,
                        stamp: ev_comp.stamp_pointer,
                        fw_stamp: ev_comp.fw_stamp_pointer,
                        stamp_value: ev_comp.value.next(),
                        stamp_slot: ev_comp.slot,
                        evctl_index: 0, // fixed
                        flush_stamps: flush_stamps as u32,
                        uuid,
                        event_seq: ev_comp.event_seq as u32,
                    }),
                    command_time: U64(0),
                    timestamp_pointers <- try_init!(fw::job::raw::TimestampPointers {
                        start_addr: Some(inner_ptr!(inner.timestamps.gpu_pointer(), start)),
                        end_addr: Some(inner_ptr!(inner.timestamps.gpu_pointer(), end)),
                    }),
                    user_timestamp_pointers: inner.user_timestamps.pointers()?,
                    client_sequence: slot_client_seq,
                    pad_2d1: Default::default(),
                    unk_2d4: 0,
                    unk_2d8: 0,
                    #[ver(V >= V13_0B4)]
                    context_store_req: U64(0),
                    #[ver(V >= V13_0B4)]
                    context_store_compl: U64(0),
                    #[ver(V >= V13_0B4)]
                    unk_2e9: Default::default(),
                    #[ver(V >= V13_0B4)]
                    unk_flag: U32(0),
                    #[ver(V >= V13_0B4)]
                    unk_pad: Default::default(),
                })
            },
        )?;

        core::mem::drop(alloc);

        fence.add_command();
        comp_job.add_cb(comp, vm_bind.slot(), move |cmd, error| {
            if let Some(err) = error {
                fence.set_error(err.into())
            }
            if let Some(mut rw) = result_writer {
                let mut result: uapi::drm_asahi_result_compute = Default::default();

                cmd.timestamps.with(|raw, _inner| {
                    result.ts_start = raw.start.load(Ordering::Relaxed);
                    result.ts_end = raw.end.load(Ordering::Relaxed);
                });

                if let Some(err) = error {
                    result.info = err.into();
                } else {
                    result.info.status = uapi::drm_asahi_status_DRM_ASAHI_STATUS_COMPLETE;
                }

                rw.write(result);
            }

            fence.command_complete();
        })?;

        notifier.threshold.with(|raw, _inner| {
            raw.increment();
        });

        comp_job.next_seq();

        Ok(())
    }
}
