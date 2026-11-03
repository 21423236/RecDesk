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

declare global {
  interface Window {
    recorderApi: {
      saveRecording: (buffer: ArrayBuffer) => Promise<SaveRecordingResult>
      createRecording: () => Promise<RecordingSession>
      appendRecordingChunk: (buffer: ArrayBuffer) => Promise<AppendResult>
      completeRecording: () => Promise<SaveRecordingResult>
      cancelRecording: () => Promise<void>
      checkRecoverableRecording: () => Promise<RecoveryInfo | null>
      recoverRecording: (tempFilePath: string) => Promise<SaveRecordingResult>
      deleteRecoverableRecording: (tempFilePath: string) => Promise<void>
      listRecoverableRecordings: () => Promise<RecoveryListResult>
      cleanupAllRecoverableRecordings: () => Promise<void>
      cleanupRecoverableRecording: (tempFilePath: string) => Promise<void>
      selectRegion: (params: RegionSelectionParams) => Promise<RegionSelectionResult>
      submitRegionSelection: (region: RegionSelection) => Promise<void>
      cancelRegionSelection: () => Promise<void>
      getRegionSelectionBackgroundFrame: () => Promise<string | null>
      showRecordingIndicator: () => Promise<void>
      hideRecordingIndicator: () => Promise<void>
      getDisplays: () => Promise<DisplayInfo[]>
      windowControl: {
        minimize: () => Promise<void>
      }
    }
  }
}

export {}
