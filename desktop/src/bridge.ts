import { invoke, isTauri } from '@tauri-apps/api/core';
import type { Desktop } from './generated/contracts';

export const nativeAvailable = () => isTauri();
export const bridge = {
  state: () => invoke<Desktop.State>('desktop_state'),
  importObject: (request: Desktop.ImportRequest) => invoke<Desktop.OperationReceipt | null>('desktop_import_object', { request }),
  start: (request: Desktop.StartRequest) => invoke<Desktop.OperationReceipt>('desktop_start', { request }),
  stop: (request: Desktop.StopRequest) => invoke<Desktop.StopResponse>('desktop_stop', { request }),
  save: (request: Desktop.SaveRequest) => invoke<Desktop.OperationReceipt | null>('desktop_save_project', { request }),
  open: () => invoke<Desktop.OperationReceipt | null>('desktop_open_project'),
  export: (request: Desktop.ExportRequest) => invoke<Desktop.OperationReceipt | null>('desktop_export', { request }),
  newProject: () => invoke<Desktop.State>('desktop_new'),
  preview: (request: Desktop.PreviewRequest) => invoke<ArrayBuffer>('desktop_read_preview', { request }),
};
