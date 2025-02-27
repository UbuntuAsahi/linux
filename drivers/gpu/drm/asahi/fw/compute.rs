// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! GPU compute job firmware structures

use super::types::*;
use super::{event, job, workqueue};
use crate::{microseq, mmu};
use kernel::sync::Arc;

pub(crate) mod raw {
    use super::*;

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct JobParameters1<'a> {
        pub(crate) preempt_buf1: GpuPointer<'a, &'a [u8]>,
        pub(crate) encoder: U64,
        pub(crate) preempt_buf2: GpuPointer<'a, &'a [u8]>,
        pub(crate) preempt_buf3: GpuPointer<'a, &'a [u8]>,
        pub(crate) preempt_buf4: GpuPointer<'a, &'a [u8]>,
        pub(crate) preempt_buf5: GpuPointer<'a, &'a [u8]>,
        pub(crate) pipeline_base: U64,
        pub(crate) unk_38: U64,
        pub(crate) helper_program: u32,
        pub(crate) unk_44: u32,
        pub(crate) helper_arg: U64,
        pub(crate) helper_cfg: u32,
        pub(crate) unk_54: u32,
        pub(crate) unk_58: u32,
        pub(crate) unk_5c: u32,
        pub(crate) iogpu_unk_40: u32,
        pub(crate) __pad: Pad<0xfc>,
    }

    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct JobParameters2<'a> {
        #[ver(V >= V13_0B4)]
        pub(crate) unk_0_0: u32,
        pub(crate) unk_0: Array<0x24, u8>,
        pub(crate) preempt_buf1: GpuPointer<'a, &'a [u8]>,
        pub(crate) encoder_end: U64,
        pub(crate) unk_34: Array<0x20, u8>,
        pub(crate) unk_g14x: u32,
        pub(crate) unk_58: u32,
        #[ver(V < V13_0B4)]
        pub(crate) unk_5c: u32,
    }

    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct RunCompute<'a> {
        pub(crate) tag: workqueue::CommandType,

        #[ver(V >= V13_0B4)]
        pub(crate) counter: U64,

        pub(crate) unk_4: u32,
        pub(crate) vm_slot: u32,
        pub(crate) notifier: GpuPointer<'a, event::Notifier::ver>,
        pub(crate) unk_pointee: u32,
        #[ver(G < G14X)]
        pub(crate) __pad0: Array<0x50, u8>,
        #[ver(G < G14X)]
        pub(crate) job_params1: JobParameters1<'a>,
        #[ver(G >= G14X)]
        pub(crate) registers: job::raw::RegisterArray,
        pub(crate) __pad1: Array<0x20, u8>,
        pub(crate) microsequence: GpuPointer<'a, &'a [u8]>,
        pub(crate) microsequence_size: u32,
        pub(crate) job_params2: JobParameters2::ver<'a>,
        pub(crate) encoder_params: job::raw::EncoderParams,
        pub(crate) meta: job::raw::JobMeta,
        pub(crate) command_time: U64,
        pub(crate) timestamp_pointers: job::raw::TimestampPointers<'a>,
        pub(crate) user_timestamp_pointers: job::raw::TimestampPointers<'a>,
        pub(crate) client_sequence: u8,
        pub(crate) pad_2d1: Array<3, u8>,
        pub(crate) unk_2d4: u32,
        pub(crate) unk_2d8: u8,
        #[ver(V >= V13_0B4)]
        pub(crate) context_store_req: U64,
        #[ver(V >= V13_0B4)]
        pub(crate) context_store_compl: U64,
        #[ver(V >= V13_0B4)]
        pub(crate) unk_2e9: Array<0x14, u8>,
        #[ver(V >= V13_0B4)]
        pub(crate) unk_flag: U32,
        #[ver(V >= V13_0B4)]
        pub(crate) unk_pad: Array<0x10, u8>,
    }
}

#[versions(AGX)]
#[derive(Debug)]
pub(crate) struct RunCompute {
    pub(crate) notifier: Arc<GpuObject<event::Notifier::ver>>,
    pub(crate) preempt_buf: GpuArray<u8>,
    pub(crate) micro_seq: microseq::MicroSequence,
    pub(crate) vm_bind: mmu::VmBind,
    pub(crate) timestamps: Arc<GpuObject<job::JobTimestamps>>,
    pub(crate) user_timestamps: job::UserTimestamps,
}

#[versions(AGX)]
impl GpuStruct for RunCompute::ver {
    type Raw<'a> = raw::RunCompute::ver<'a>;
}

#[versions(AGX)]
impl workqueue::Command for RunCompute::ver {}
