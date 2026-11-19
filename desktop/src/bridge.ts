import { invoke, isTauri } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import type { Desktop } from './generated/contracts';

export const nativeAvailable = () => isTauri();
export const bridge = {
  state: () => invoke<Desktop.State>('desktop_state'),
  importObject: (request: Desktop.ImportRequest) => invoke<Desktop.OperationReceipt | null>('desktop_import_object', { request }),
  // The window reports that files were dropped; Rust keeps their paths.
  onDrop: (handler: () => void) => listen('desktop-drop', handler),
  importDropped: (request: Desktop.ImportRequest) => invoke<Desktop.OperationReceipt>('desktop_import_dropped', { request }),
  repair: (request: Desktop.RepairRequest) => invoke<Desktop.OperationReceipt>('desktop_repair', { request }),
  start: (request: Desktop.StartRequest) => invoke<Desktop.OperationReceipt>('desktop_start', { request }),
  // More budget for the displayed result: the engine re-checks its poses and searches on from them.
  continue: (request: Desktop.StartRequest) => invoke<Desktop.OperationReceipt>('desktop_continue', { request }),
  stop: (request: Desktop.StopRequest) => invoke<Desktop.StopResponse>('desktop_stop', { request }),
  // Save writes the current project, or asks for a file when there is none; Save As always asks.
  save: (request: Desktop.SaveRequest) => invoke<Desktop.OperationReceipt | null>('desktop_save_project', { request }),
  saveAs: (request: Desktop.SaveRequest) => invoke<Desktop.OperationReceipt | null>('desktop_save_project_as', { request }),
  open: () => invoke<Desktop.OperationReceipt | null>('desktop_open_project'),
  export: (request: Desktop.ExportRequest) => invoke<Desktop.OperationReceipt | null>('desktop_export', { request }),
  // The viewer's PNG for the displayed result; Rust asks where to write it and adds the result's count and settings.
  exportPng: (resultId: string, png: Uint8Array) => invoke<Desktop.OperationReceipt | null>('desktop_export_png', png, { headers: { 'spectrapack-result': resultId } }),
  // Shows the last saved or exported file in Explorer; Rust knows which.
  reveal: () => invoke<void>('desktop_reveal'),
  newProject: () => invoke<Desktop.State>('desktop_new'),
  preview: (request: Desktop.PreviewRequest) => invoke<ArrayBuffer>('desktop_read_preview', { request }),
};
