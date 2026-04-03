import { contextBridge, ipcRenderer } from 'electron'

export type SaveRecordingResult = {
  canceled: boolean
  filePath?: string
}

export interface RegionSelection {
  x: number
  y: number
  width: number
  height: number
}

export interface DisplayBounds {
  x: number
  y: number
  width: number
  height: number
}

export type RegionSelectionResult = {
  canceled: boolean
  region?: RegionSelection
  displayBounds?: DisplayBounds
}

export interface RegionSelectionParams {
  backgroundFrameDataUrl: string
}

export type RecordingSession = {
  tempFilePath: string
  createdAt: number
}

export type AppendResult = {
  success: boolean
  bytesWritten: number
}

export type RecoveryInfo = {
  tempFilePath: string
  size: number
  createdAt: number
}

export type RecoveryFileInfo = {
  tempFilePath: string
  size: number
  createdAt: number
}

export type RecoveryListResult = {
  files: RecoveryFileInfo[]
  totalCount: number
  totalSize: number
}

export type DisplayInfo = {
  id: number
  bounds: DisplayBounds
  scaleFactor: number
}

const recorderApi = {
  saveRecording: (buffer: ArrayBuffer): Promise<SaveRecordingResult> => {
    return ipcRenderer.invoke('recording:save', { buffer }) as Promise<SaveRecordingResult>
  },
  createRecording: (): Promise<RecordingSession> => {
    return ipcRenderer.invoke('recording:create') as Promise<RecordingSession>
  },
  appendRecordingChunk: (buffer: ArrayBuffer): Promise<AppendResult> => {
    return ipcRenderer.invoke('recording:append', { buffer }) as Promise<AppendResult>
  },
  completeRecording: (): Promise<SaveRecordingResult> => {
    return ipcRenderer.invoke('recording:complete') as Promise<SaveRecordingResult>
  },
  cancelRecording: (): Promise<void> => {
    return ipcRenderer.invoke('recording:cancel') as Promise<void>
  },
  checkRecoverableRecording: (): Promise<RecoveryInfo | null> => {
    return ipcRenderer.invoke('recording:check-recovery') as Promise<RecoveryInfo | null>
  },
  recoverRecording: (tempFilePath: string): Promise<SaveRecordingResult> => {
    return ipcRenderer.invoke('recording:recover', { tempFilePath }) as Promise<SaveRecordingResult>
  },
  deleteRecoverableRecording: (tempFilePath: string): Promise<void> => {
    return ipcRenderer.invoke('recording:delete-recovery', { tempFilePath }) as Promise<void>
  },
  listRecoverableRecordings: (): Promise<RecoveryListResult> => {
    return ipcRenderer.invoke('recording:list-recovery') as Promise<RecoveryListResult>
  },
  cleanupAllRecoverableRecordings: (): Promise<void> => {
    return ipcRenderer.invoke('recording:cleanup-all-recovery') as Promise<void>
  },
  cleanupRecoverableRecording: (tempFilePath: string): Promise<void> => {
    return ipcRenderer.invoke('recording:cleanup-recovery', { tempFilePath }) as Promise<void>
  },
  selectRegion: (params: RegionSelectionParams): Promise<RegionSelectionResult> => {
    return ipcRenderer.invoke('region:select', params) as Promise<RegionSelectionResult>
  },
  submitRegionSelection: (region: RegionSelection): Promise<void> => {
    return ipcRenderer.invoke('region-selection:submit', { region }) as Promise<void>
  },
  cancelRegionSelection: (): Promise<void> => {
    return ipcRenderer.invoke('region-selection:cancel') as Promise<void>
  },
  getRegionSelectionBackgroundFrame: (): Promise<string | null> => {
    return ipcRenderer.invoke('region:background-frame:get') as Promise<string | null>
  },
  showRecordingIndicator: (): Promise<void> => {
    return ipcRenderer.invoke('recording-indicator:show') as Promise<void>
  },
  hideRecordingIndicator: (): Promise<void> => {
    return ipcRenderer.invoke('recording-indicator:hide') as Promise<void>
  },
  getDisplays: (): Promise<DisplayInfo[]> => {
    return ipcRenderer.invoke('screen:get-displays') as Promise<DisplayInfo[]>
  }
}

contextBridge.exposeInMainWorld('recorderApi', recorderApi)
