//! Establish owner job membership before any native spawn. Windows associates
//! later children with this job atomically during process creation. Cooperative
//! Stop still uses its marker; OS owner exit closes the non-inherited job handle.
use crate::model::{error, Outcome};
use std::sync::OnceLock;
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE},
    System::{JobObjects::*, Threading::GetCurrentProcess},
};

struct NativeJob(HANDLE);
static OWNER_JOB: OnceLock<Outcome<()>> = OnceLock::new();

pub(crate) fn bind_owner() -> Outcome<()> {
    OWNER_JOB
        .get_or_init(|| {
            let job = NativeJob::new()?;
            unsafe {
                if AssignProcessToJobObject(job.0, GetCurrentProcess()) == 0 {
                    return Err(error(
                        "ENGINE_START",
                        std::io::Error::last_os_error().to_string(),
                    ));
                }
            }
            // Closing the associated handle would also kill the owner. Retain
            // it for the entire OS process lifetime, independently of Core
            // drops and error paths. Null security attributes forbid inheritance.
            std::mem::forget(job);
            Ok(())
        })
        .clone()
}

impl NativeJob {
    fn new() -> Outcome<Self> {
        unsafe {
            let handle = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if handle.is_null() {
                return Err(error(
                    "ENGINE_START",
                    std::io::Error::last_os_error().to_string(),
                ));
            }
            let job = Self(handle);
            let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                &limits as *const _ as *const _,
                std::mem::size_of_val(&limits) as u32,
            ) == 0
            {
                return Err(error(
                    "ENGINE_START",
                    std::io::Error::last_os_error().to_string(),
                ));
            }
            Ok(job)
        }
    }
}
impl Drop for NativeJob {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}
