import { app, BrowserWindow, desktopCapturer, dialog, ipcMain, screen, session } from 'electron'
import ffmpegPath from 'ffmpeg-static'
import { spawn } from 'node:child_process'
import { createWriteStream, mkdir, readdir, readFile, stat, unlink } from 'node:fs'
import { mkdtemp, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { pathToFileURL } from 'node:url'
import type { Writable } from 'node:stream'

type RegionSelection = {
  x: number
  y: number
  width: number
  height: number
}

type DisplayBounds = {
  x: number
  y: number
  width: number
  height: number
}

type RegionSelectionResult = {
  canceled: boolean
  region?: RegionSelection
  displayBounds?: DisplayBounds
}

type RegionSelectionParams = {
  backgroundFrameDataUrl: string
}

type RecordingIndicatorParams = {
  startTime: number
}

type RecordingSession = {
  tempDir: string
  tempFilePath: string
  createdAt: number
  writeStream: Writable | null
}

type RecoveryInfo = {
  tempFilePath: string
  size: number
  createdAt: number
}

type RecoveryFileInfo = {
  tempFilePath: string
  size: number
  createdAt: number
  markerPath: string
}

type RecoveryListResult = {
  files: RecoveryFileInfo[]
  totalCount: number
  totalSize: number
}

type DisplayInfo = {
  id: number
  bounds: DisplayBounds
  scaleFactor: number
}

let mainWindow: BrowserWindow | null = null
let regionSelectionWindows: BrowserWindow[] = []
let recordingIndicatorWindow: BrowserWindow | null = null
let recordingStartTime: number = 0
let pendingRegionSelection:
  | {
    resolve: (result: RegionSelectionResult) => void
    reject: (reason?: unknown) => void
  }
  | null = null
let currentBackgroundFrame: string | null = null
let currentRecordingSession: RecordingSession | null = null
const RECOVERY_DIR = join(app.getPath('userData'), 'recording-recovery')

const ensureMp4Extension = (filePath: string): string => {
  return filePath.toLowerCase().endsWith('.mp4') ? filePath : `${filePath}.mp4`
}

const getResolvedFfmpegPath = (): string => {
  if (!ffmpegPath) {
    throw new Error('FFmpeg binary is unavailable.')
  }

  return app.isPackaged ? ffmpegPath.replace('app.asar', 'app.asar.unpacked') : ffmpegPath
}

const cleanupRecordingSession = async (session: RecordingSession): Promise<void> => {
  if (session.writeStream && !session.writeStream.destroyed) {
    session.writeStream.destroy()
  }
  await rm(session.tempDir, { recursive: true, force: true })
}

const convertWebmToMp4 = async (inputPath: string, outputPath: string): Promise<void> => {
  await new Promise<void>((resolve, reject) => {
    const ffmpegProcess = spawn(getResolvedFfmpegPath(), [
      '-y',
      '-i',
      inputPath,
      '-map',
      '0:v:0',
      '-map',
      '0:a:0?',
      '-c:v',
      'libx264',
      '-preset',
      'ultrafast',
      '-pix_fmt',
      'yuv420p',
      '-movflags',
      '+faststart',
      '-c:a',
      'aac',
      '-b:a',
      '128k',
      outputPath
    ])

    let ffmpegErrorOutput = ''

    ffmpegProcess.stderr.on('data', (chunk: Buffer) => {
      ffmpegErrorOutput += chunk.toString()
    })

    ffmpegProcess.on('error', (error) => {
      reject(error)
    })

    ffmpegProcess.on('close', (code) => {
      if (code === 0) {
        resolve()
        return
      }

      reject(new Error(ffmpegErrorOutput || `FFmpeg exited with code ${code ?? 'unknown'}.`))
    })
  })
}

const deleteRecoveryMarker = async (tempFilePath: string): Promise<void> => {
  try {
    const markerFiles = await new Promise<string[]>((resolve, reject) => {
      readdir(RECOVERY_DIR, (error, files) => {
        if (error) reject(error)
        else resolve(files)
      })
    })

    for (const file of markerFiles) {
      const markerPath = join(RECOVERY_DIR, file)
      try {
        const content = await new Promise<string>((resolve, reject) => {
          readFile(markerPath, 'utf-8', (error, data) => {
            if (error) reject(error)
            else resolve(data)
          })
        })
        const data = JSON.parse(content)
        if (data.tempFilePath === tempFilePath) {
          await new Promise<void>((resolve, reject) => {
            unlink(markerPath, (error) => {
              if (error) reject(error)
              else resolve()
            })
          })
        }
      } catch {}
    }
  } catch {}
}

const getRecoveryFilesList = async (): Promise<RecoveryListResult> => {
  const files: RecoveryFileInfo[] = []
  
  try {
    const markerFiles = await new Promise<string[]>((resolve, reject) => {
      readdir(RECOVERY_DIR, (error, files) => {
        if (error) reject(error)
        else resolve(files)
      })
    })
    
    for (const file of markerFiles) {
      const markerPath = join(RECOVERY_DIR, file)
      try {
        const content = await new Promise<string>((resolve, reject) => {
          readFile(markerPath, 'utf-8', (error, data) => {
            if (error) reject(error)
            else resolve(data)
          })
        })
        const data = JSON.parse(content)
        
        try {
          const stats = await new Promise<{ size: number }>((resolve, reject) => {
            stat(data.tempFilePath, (error, stats) => {
              if (error) reject(error)
              else resolve({ size: stats.size })
            })
          })
          
          files.push({
            tempFilePath: data.tempFilePath,
            size: stats.size,
            createdAt: data.createdAt,
            markerPath: markerPath
          })
        } catch {
          await new Promise<void>((resolve, reject) => {
            unlink(markerPath, (error) => {
              if (error) reject(error)
              else resolve()
            })
          })
        }
      } catch {}
    }
  } catch {}
  
  const totalSize = files.reduce((sum, file) => sum + file.size, 0)
  
  return {
    files,
    totalCount: files.length,
    totalSize
  }
}

const getRendererUrl = (search = ''): string => {
  if (process.env.ELECTRON_RENDERER_URL) {
    return `${process.env.ELECTRON_RENDERER_URL}${search}`
  }

  return `${pathToFileURL(join(__dirname, '../renderer/index.html')).toString()}${search}`
}

const closeRegionSelectionWindows = (): void => {
  const windowsToClose = regionSelectionWindows
  regionSelectionWindows = []

  windowsToClose.forEach((window) => {
    if (!window.isDestroyed()) {
      window.close()
    }
  })
}

const closeRecordingIndicatorWindow = (): void => {
  if (recordingIndicatorWindow && !recordingIndicatorWindow.isDestroyed()) {
    recordingIndicatorWindow.close()
  }
  recordingIndicatorWindow = null
}

const createRecordingIndicatorWindow = async (
  startTime: number
): Promise<void> => {
  closeRecordingIndicatorWindow()

  const primaryDisplay = screen.getPrimaryDisplay()
  const displayBounds = primaryDisplay.bounds

  const windowWidth = 160
  const windowHeight = 44

  const indicatorWindow = new BrowserWindow({
    x: displayBounds.x + displayBounds.width - windowWidth - 20,
    y: displayBounds.y + 20,
    width: windowWidth,
    height: windowHeight,
    frame: false,
    transparent: true,
    resizable: false,
    movable: true,
    minimizable: false,
    maximizable: false,
    fullscreenable: false,
    skipTaskbar: true,
    alwaysOnTop: true,
    hasShadow: false,
    backgroundColor: '#00000000',
    focusable: false,
    webPreferences: {
      preload: join(__dirname, '../preload/index.mjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    },
    show: false
  })

  indicatorWindow.setAlwaysOnTop(true, 'floating')
  indicatorWindow.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true })

  indicatorWindow.on('closed', () => {
    if (recordingIndicatorWindow === indicatorWindow) {
      recordingIndicatorWindow = null
    }
  })

  const params = new URLSearchParams({
    overlay: 'recording-indicator',
    startTime: String(startTime)
  })
  await indicatorWindow.loadURL(getRendererUrl(`?${params.toString()}`))

  recordingIndicatorWindow = indicatorWindow
  recordingStartTime = startTime
  indicatorWindow.showInactive()
}

const completeRegionSelection = (result: RegionSelectionResult): void => {
  const pending = pendingRegionSelection

  pendingRegionSelection = null
  currentBackgroundFrame = null
  closeRegionSelectionWindows()

  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.focus()
  }

  pending?.resolve(result)
}

const createMainWindow = async (): Promise<void> => {
  mainWindow = new BrowserWindow({
    width: 1180,
    height: 860,
    minWidth: 1000,
    minHeight: 720,
    title: 'RecDesk',
    autoHideMenuBar: true,
    webPreferences: {
      preload: join(__dirname, '../preload/index.mjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  })

  if (process.env.ELECTRON_RENDERER_URL) {
    await mainWindow.loadURL(getRendererUrl())
    // mainWindow.webContents.openDevTools({ mode: 'detach' })
    return
  }

  await mainWindow.loadURL(getRendererUrl())
}

const createRegionSelectionWindows = async (): Promise<void> => {
  const displays = screen.getAllDisplays()
  const overlayWindows: BrowserWindow[] = []

  for (const display of displays) {
    const overlayWindow = new BrowserWindow({
      x: display.bounds.x,
      y: display.bounds.y,
      width: display.bounds.width,
      height: display.bounds.height,
      frame: false,
      transparent: true,
      resizable: false,
      movable: false,
      minimizable: false,
      maximizable: false,
      fullscreenable: false,
      skipTaskbar: true,
      alwaysOnTop: true,
      hasShadow: false,
      backgroundColor: '#00000000',
      webPreferences: {
        preload: join(__dirname, '../preload/index.mjs'),
        contextIsolation: true,
        nodeIntegration: false,
        sandbox: false
      },
      show: false
    })

    overlayWindow.setAlwaysOnTop(true, 'screen-saver')
    overlayWindow.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true })

    overlayWindow.on('closed', () => {
      regionSelectionWindows = regionSelectionWindows.filter((window) => window !== overlayWindow)

      if (pendingRegionSelection) {
        completeRegionSelection({ canceled: true })
      }
    })

    await overlayWindow.loadURL(getRendererUrl('?overlay=region-selection'))
    overlayWindows.push(overlayWindow)
  }

  regionSelectionWindows = overlayWindows

  for (const window of regionSelectionWindows) {
    window.show()
  }
  regionSelectionWindows[0]?.focus()
}

ipcMain.handle('recording:save', async (_event, payload: { buffer: ArrayBuffer }) => {
  const temporaryDirectoryPath = await mkdtemp(join(tmpdir(), 'screen-recording-'))
  const temporaryWebmPath = join(temporaryDirectoryPath, 'recording.webm')

  await writeFile(temporaryWebmPath, Buffer.from(payload.buffer))

  const { canceled, filePath } = await dialog.showSaveDialog({
    title: 'Export recording as MP4',
    defaultPath: `screen-recording-${Date.now()}.mp4`,
    filters: [{ name: 'MP4 Video', extensions: ['mp4'] }]
  })

  if (canceled || !filePath) {
    await rm(temporaryDirectoryPath, { recursive: true, force: true })
    return { canceled: true }
  }

  const finalOutputPath = ensureMp4Extension(filePath)

  try {
    await convertWebmToMp4(temporaryWebmPath, finalOutputPath)
  } finally {
    await rm(temporaryDirectoryPath, { recursive: true, force: true })
  }

  return {
    canceled: false,
    filePath: finalOutputPath
  }
})

ipcMain.handle('recording:create', async () => {
  if (currentRecordingSession) {
    await cleanupRecordingSession(currentRecordingSession)
  }

  const tempDir = await mkdtemp(join(tmpdir(), 'screen-recording-'))
  const tempFilePath = join(tempDir, 'recording.webm')
  const writeStream = createWriteStream(tempFilePath, { flags: 'a' })

  currentRecordingSession = {
    tempDir,
    tempFilePath,
    createdAt: Date.now(),
    writeStream
  }

  await new Promise<void>((resolve, reject) => {
    mkdir(RECOVERY_DIR, { recursive: true }, (error) => {
      if (error) reject(error)
      else resolve()
    })
  })

  const recoveryMarker = join(RECOVERY_DIR, `${Date.now()}.marker`)
  await writeFile(recoveryMarker, JSON.stringify({
    tempFilePath: currentRecordingSession.tempFilePath,
    createdAt: currentRecordingSession.createdAt
  }))

  return {
    tempFilePath: currentRecordingSession.tempFilePath,
    createdAt: currentRecordingSession.createdAt
  }
})

ipcMain.handle('recording:append', async (_event, { buffer }: { buffer: ArrayBuffer }) => {
  if (!currentRecordingSession || !currentRecordingSession.writeStream) {
    throw new Error('No active recording session')
  }

  const bufferData = Buffer.from(buffer)

  await new Promise<void>((resolve, reject) => {
    currentRecordingSession!.writeStream!.write(bufferData, (error) => {
      if (error) reject(error)
      else resolve()
    })
  })

  const stats = await new Promise<{ size: number }>((resolve, reject) => {
    stat(currentRecordingSession!.tempFilePath, (error, stats) => {
      if (error) reject(error)
      else resolve({ size: stats.size })
    })
  })

  return {
    success: true,
    bytesWritten: bufferData.length
  }
})

ipcMain.handle('recording:complete', async () => {
  if (!currentRecordingSession) {
    throw new Error('No active recording session')
  }

  if (currentRecordingSession.writeStream) {
    await new Promise<void>((resolve) => {
      currentRecordingSession!.writeStream!.end(() => resolve())
    })
  }

  await deleteRecoveryMarker(currentRecordingSession.tempFilePath)

  const { canceled, filePath } = await dialog.showSaveDialog({
    title: 'Export recording as MP4',
    defaultPath: `screen-recording-${Date.now()}.mp4`,
    filters: [{ name: 'MP4 Video', extensions: ['mp4'] }]
  })

  if (canceled || !filePath) {
    await cleanupRecordingSession(currentRecordingSession)
    currentRecordingSession = null
    return { canceled: true }
  }

  const finalOutputPath = ensureMp4Extension(filePath)

  try {
    await convertWebmToMp4(currentRecordingSession.tempFilePath, finalOutputPath)
  } finally {
    await cleanupRecordingSession(currentRecordingSession)
    currentRecordingSession = null
  }

  return {
    canceled: false,
    filePath: finalOutputPath
  }
})

ipcMain.handle('recording:cancel', async () => {
  if (currentRecordingSession) {
    await cleanupRecordingSession(currentRecordingSession)
    currentRecordingSession = null
  }
})

ipcMain.handle('recording:check-recovery', async () => {
  try {
    const markerFiles = await new Promise<string[]>((resolve, reject) => {
      readdir(RECOVERY_DIR, (error, files) => {
        if (error) reject(error)
        else resolve(files)
      })
    })

    for (const file of markerFiles) {
      const markerPath = join(RECOVERY_DIR, file)
      try {
        const content = await new Promise<string>((resolve, reject) => {
          readFile(markerPath, 'utf-8', (error, data) => {
            if (error) reject(error)
            else resolve(data)
          })
        })
        const data = JSON.parse(content)
        const stats = await new Promise<{ size: number }>((resolve, reject) => {
          stat(data.tempFilePath, (error, stats) => {
            if (error) reject(error)
            else resolve({ size: stats.size })
          })
        })

        return {
          tempFilePath: data.tempFilePath,
          size: stats.size,
          createdAt: data.createdAt
        }
      } catch {
        await new Promise<void>((resolve, reject) => {
          unlink(markerPath, (error) => {
            if (error) reject(error)
            else resolve()
          })
        })
      }
    }
  } catch {}

  return null
})

ipcMain.handle('recording:recover', async (_event, { tempFilePath }: { tempFilePath: string }) => {
  const { canceled, filePath } = await dialog.showSaveDialog({
    title: 'Recover recording as MP4',
    defaultPath: `recovered-recording-${Date.now()}.mp4`,
    filters: [{ name: 'MP4 Video', extensions: ['mp4'] }]
  })

  if (canceled || !filePath) {
    return { canceled: true }
  }

  const finalOutputPath = ensureMp4Extension(filePath)
  const tempDir = dirname(tempFilePath)

  try {
    await convertWebmToMp4(tempFilePath, finalOutputPath)
  } finally {
    await rm(tempDir, { recursive: true, force: true })
    await deleteRecoveryMarker(tempFilePath)
  }

  return {
    canceled: false,
    filePath: finalOutputPath
  }
})

ipcMain.handle('recording:delete-recovery', async (_event, { tempFilePath }: { tempFilePath: string }) => {
  const tempDir = dirname(tempFilePath)
  await rm(tempDir, { recursive: true, force: true })
  await deleteRecoveryMarker(tempFilePath)
})

ipcMain.handle('recording:list-recovery', async (): Promise<RecoveryListResult> => {
  return await getRecoveryFilesList()
})

ipcMain.handle('recording:cleanup-all-recovery', async (): Promise<void> => {
  const list = await getRecoveryFilesList()
  
  for (const file of list.files) {
    const tempDir = dirname(file.tempFilePath)
    await rm(tempDir, { recursive: true, force: true })
    await new Promise<void>((resolve, reject) => {
      unlink(file.markerPath, (error) => {
        if (error) reject(error)
        else resolve()
      })
    })
  }
})

ipcMain.handle('recording:cleanup-recovery', async (_event, { tempFilePath }: { tempFilePath: string }): Promise<void> => {
  const list = await getRecoveryFilesList()
  const file = list.files.find(f => f.tempFilePath === tempFilePath)
  
  if (file) {
    const tempDir = dirname(file.tempFilePath)
    await rm(tempDir, { recursive: true, force: true })
    await new Promise<void>((resolve, reject) => {
      unlink(file.markerPath, (error) => {
        if (error) reject(error)
        else resolve()
      })
    })
  }
})

ipcMain.handle('screen:get-displays', (): DisplayInfo[] => {
  const displays = screen.getAllDisplays()
  return displays.map(display => ({
    id: display.id,
    bounds: {
      x: display.bounds.x,
      y: display.bounds.y,
      width: display.bounds.width,
      height: display.bounds.height
    },
    scaleFactor: display.scaleFactor
  }))
})

ipcMain.handle('region:select', async (_event, params: RegionSelectionParams) => {
  if (pendingRegionSelection) {
    completeRegionSelection({ canceled: true })
  }

  currentBackgroundFrame = params.backgroundFrameDataUrl

  return await new Promise<RegionSelectionResult>((resolve, reject) => {
    pendingRegionSelection = { resolve, reject }

    void createRegionSelectionWindows().catch((error) => {
      pendingRegionSelection = null
      currentBackgroundFrame = null
      closeRegionSelectionWindows()
      reject(error)
    })
  })
})

ipcMain.handle('region:background-frame:get', async () => {
  return currentBackgroundFrame
})

ipcMain.handle('region-selection:submit', async (_event, payload: { region: RegionSelection }) => {
  const overlayWindow = BrowserWindow.fromWebContents(_event.sender)
  const matchedDisplay = overlayWindow
    ? screen.getDisplayMatching(overlayWindow.getBounds())
    : screen.getPrimaryDisplay()

  completeRegionSelection({
    canceled: false,
    region: payload.region,
    displayBounds: {
      x: matchedDisplay.bounds.x,
      y: matchedDisplay.bounds.y,
      width: matchedDisplay.bounds.width,
      height: matchedDisplay.bounds.height
    }
  })
})

ipcMain.handle('region-selection:cancel', async () => {
  completeRegionSelection({ canceled: true })
})

ipcMain.handle('recording-indicator:show', async () => {
  await createRecordingIndicatorWindow(Date.now())
})

ipcMain.handle('recording-indicator:hide', async () => {
  closeRecordingIndicatorWindow()
})

ipcMain.handle('window:minimize', async () => {
  mainWindow?.minimize()
})

app.whenReady().then(async () => {
  session.defaultSession.setDisplayMediaRequestHandler(
    async (_request, callback) => {
      const sources = await desktopCapturer.getSources({
        types: ['screen', 'window'],
        thumbnailSize: { width: 0, height: 0 }
      })

      const screenSources = sources.filter(s => s.id.startsWith('screen:'))
      const primarySource = screenSources[0] || sources[0]

      if (!primarySource) {
        callback({})
        return
      }

      callback({
        video: primarySource,
        audio: 'loopback'
      })
    },
    {
      useSystemPicker: true
    }
  )

  await createMainWindow()

  app.on('activate', async () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      await createMainWindow()
    }
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})

app.on('before-quit', async () => {
  if (currentRecordingSession) {
    await cleanupRecordingSession(currentRecordingSession)
  }
})
