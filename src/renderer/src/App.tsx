import { useCallback, useEffect, useMemo, useRef, useState } from 'react'

type RecorderState = 'idle' | 'selecting' | 'region-select' | 'recording' | 'saving' | 'error'
type CaptureMode = 'full' | 'region'

interface ResolutionOption {
  name: string
  width: number
  height: number
}

interface RecordingParams {
  resolution: string
  frameRate: number
  audioEnabled: boolean
  microphoneEnabled: boolean
  captureMode: CaptureMode
}

interface Region {
  x: number
  y: number
  width: number
  height: number
}

interface DisplayBounds {
  x: number
  y: number
  width: number
  height: number
}

interface RegionSelection {
  x: number
  y: number
  width: number
  height: number
}

interface RecoveryFileInfo {
  tempFilePath: string
  size: number
  createdAt: number
}

const MIN_REGION_SIZE = 50

const preferredMimeTypes = ['video/webm;codecs=vp9', 'video/webm;codecs=vp8', 'video/webm']

const FLUSH_SIZE_THRESHOLD = 5 * 1024 * 1024
const FLUSH_TIME_THRESHOLD = 2000

const ALL_RESOLUTION_PRESETS: ResolutionOption[] = [
  { name: '4K', width: 3840, height: 2160 },
  { name: '2K', width: 2560, height: 1440 },
  { name: '1080p', width: 1920, height: 1080 },
  { name: '720p', width: 1280, height: 720 },
  { name: '480p', width: 854, height: 480 }
]

const generateResolutionOptions = (maxWidth: number, maxHeight: number): ResolutionOption[] => {
  return ALL_RESOLUTION_PRESETS.filter(
    preset => preset.width <= maxWidth && preset.height <= maxHeight
  )
}

const getSupportedMimeType = (): string | undefined => {
  return preferredMimeTypes.find((mimeType) => MediaRecorder.isTypeSupported(mimeType))
}

const stopTracks = (stream: MediaStream | null): void => {
  stream?.getTracks().forEach((track) => track.stop())
}

const normalizeRegion = (region: Region, maxWidth: number, maxHeight: number): Region | null => {
  const x = Math.max(0, Math.min(region.x, maxWidth - 1))
  const y = Math.max(0, Math.min(region.y, maxHeight - 1))

  const availableWidth = maxWidth - x
  const availableHeight = maxHeight - y

  let width = Math.min(region.width, availableWidth)
  let height = Math.min(region.height, availableHeight)

  width -= width % 2
  height -= height % 2

  if (width < MIN_REGION_SIZE || height < MIN_REGION_SIZE) {
    return null
  }

  return { x, y, width, height }
}

const waitForVideoReady = async (video: HTMLVideoElement): Promise<void> => {
  if (video.videoWidth > 0 && video.videoHeight > 0 && video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA) {
    return
  }

  await new Promise<void>((resolve) => {
    const handleReady = (): void => {
      video.removeEventListener('loadedmetadata', handleReady)
      video.removeEventListener('canplay', handleReady)
      resolve()
    }

    video.addEventListener('loadedmetadata', handleReady)
    video.addEventListener('canplay', handleReady)
  })
}

const mapSelectionToVideoRegion = (
  selection: RegionSelection,
  displayBounds: DisplayBounds,
  videoWidth: number,
  videoHeight: number
): Region | null => {
  const scaleX = videoWidth / displayBounds.width
  const scaleY = videoHeight / displayBounds.height

  return normalizeRegion(
    {
      x: Math.round(selection.x * scaleX),
      y: Math.round(selection.y * scaleY),
      width: Math.round(selection.width * scaleX),
      height: Math.round(selection.height * scaleY)
    },
    videoWidth,
    videoHeight
  )
}

const getMixedAudioTrack = async (
  displayStream: MediaStream | null,
  micStream: MediaStream | null,
  audioContextRef: React.MutableRefObject<AudioContext | null>
): Promise<MediaStreamTrack | null> => {
  const displayAudioTracks = displayStream?.getAudioTracks() ?? []
  const micAudioTracks = micStream?.getAudioTracks() ?? []

  if (displayAudioTracks.length === 0 && micAudioTracks.length === 0) {
    return null
  }

  if (displayAudioTracks.length > 0 && micAudioTracks.length === 0) {
    return displayAudioTracks[0]
  }

  if (displayAudioTracks.length === 0 && micAudioTracks.length > 0) {
    return micAudioTracks[0]
  }

  if (audioContextRef.current) {
    await audioContextRef.current.close()
    audioContextRef.current = null
  }

  const audioContext = new AudioContext()
  audioContextRef.current = audioContext

  if (audioContext.state === 'suspended') {
    await audioContext.resume()
  }

  const destination = audioContext.createMediaStreamDestination()

  const displaySource = audioContext.createMediaStreamSource(
    new MediaStream([displayAudioTracks[0]])
  )
  displaySource.connect(destination)

  const micSource = audioContext.createMediaStreamSource(
    new MediaStream([micAudioTracks[0]])
  )
  micSource.connect(destination)

  const mixedTracks = destination.stream.getAudioTracks()
  if (mixedTracks.length > 0) {
    return mixedTracks[0]
  }

  await audioContext.close()
  audioContextRef.current = null
  return null
}

function App() {
  const [status, setStatus] = useState<RecorderState>('idle')
  const [errorMessage, setErrorMessage] = useState('')
  const [savedPath, setSavedPath] = useState('')
  const [resolutionOptions, setResolutionOptions] = useState<ResolutionOption[]>(ALL_RESOLUTION_PRESETS)
  const [params, setParams] = useState<RecordingParams>({
    resolution: '1080p',
    frameRate: 30,
    audioEnabled: true,
    microphoneEnabled: false,
    captureMode: 'full'
  })
  const [region, setRegion] = useState<Region | null>(null)
  const [recoveryFiles, setRecoveryFiles] = useState<RecoveryFileInfo[]>([])
  const [recoveryTotalSize, setRecoveryTotalSize] = useState(0)
  const [recoveryLoading, setRecoveryLoading] = useState(false)

  useEffect(() => {
    const fetchDisplays = async (): Promise<void> => {
      try {
        const displays = await window.recorderApi.getDisplays()
        if (displays.length > 0) {
          const primaryDisplay = displays.find(d => d.bounds.x === 0 && d.bounds.y === 0) || displays[0]
          const options = generateResolutionOptions(primaryDisplay.bounds.width, primaryDisplay.bounds.height)
          setResolutionOptions(options)
          if (options.length > 0 && !options.find(o => o.name === params.resolution)) {
            setParams(current => ({ ...current, resolution: options[0].name }))
          }
        }
      } catch (error) {
        console.error('Failed to get display info:', error)
      }
    }
    fetchDisplays()
  }, [])

  const mediaRecorderRef = useRef<MediaRecorder | null>(null)
  const streamRef = useRef<MediaStream | null>(null)
  const chunksRef = useRef<Blob[]>([])
  const captureVideoRef = useRef<HTMLVideoElement | null>(null)
  const canvasRef = useRef<HTMLCanvasElement | null>(null)
  const canvasStreamRef = useRef<MediaStream | null>(null)
  const animationFrameRef = useRef<number | null>(null)
  const recordingErrorRef = useRef(false)
  const micStreamRef = useRef<MediaStream | null>(null)
  const audioContextRef = useRef<AudioContext | null>(null)
  const pendingChunksRef = useRef<Blob[]>([])
  const lastFlushTimeRef = useRef<number>(0)
  const sessionRef = useRef<{ tempFilePath: string } | null>(null)
  const recordingStartTimeRef = useRef<number>(0)
  const flushTimerRef = useRef<number | null>(null)

  const statusText = useMemo(() => {
    switch (status) {
      case 'selecting':
        return '正在获取屏幕源...'
      case 'region-select':
        return '请直接在屏幕上拖动选择录制区域'
      case 'recording':
        return params.captureMode === 'region' && region
          ? `正在录制选定区域 (${region.width}x${region.height})...`
          : '正在录制桌面内容...'
      case 'saving':
        return '正在保存录制文件...'
      case 'error':
        return '录制失败，请重试。'
      default:
        return '配置录制参数后，点击开始录制选择屏幕。'
    }
  }, [params.captureMode, region, status])

  const flushPendingChunks = useCallback(async (): Promise<void> => {
    if (pendingChunksRef.current.length === 0 || !sessionRef.current) {
      return
    }

    const blob = new Blob(pendingChunksRef.current, {
      type: mediaRecorderRef.current?.mimeType || 'video/webm'
    })
    pendingChunksRef.current = []

    const buffer = await blob.arrayBuffer()

    try {
      await window.recorderApi.appendRecordingChunk(buffer)
      lastFlushTimeRef.current = Date.now()
    } catch (error) {
      console.error('Failed to append chunk:', error)
      throw error
    }
  }, [])

  const clearFlushTimer = useCallback(() => {
    if (flushTimerRef.current) {
      clearInterval(flushTimerRef.current)
      flushTimerRef.current = null
    }
  }, [])

  const refreshRecoveryFiles = useCallback(async (): Promise<void> => {
    setRecoveryLoading(true)
    try {
      const result = await window.recorderApi.listRecoverableRecordings()
      setRecoveryFiles(result.files)
      setRecoveryTotalSize(result.totalSize)
    } catch (error) {
      console.error('Failed to list recovery files:', error)
    } finally {
      setRecoveryLoading(false)
    }
  }, [])

  const handleCleanupAll = useCallback(async (): Promise<void> => {
    if (recoveryFiles.length === 0) return

    const totalMB = (recoveryTotalSize / 1024 / 1024).toFixed(2)
    const confirmMessage = `确认删除所有 ${recoveryFiles.length} 个临时录制文件？\n\n总大小：${totalMB} MB\n\n此操作不可撤销。`

    if (!window.confirm(confirmMessage)) return

    setRecoveryLoading(true)
    try {
      await window.recorderApi.cleanupAllRecoverableRecordings()
      setRecoveryFiles([])
      setRecoveryTotalSize(0)
    } catch (error) {
      window.alert('清理失败：' + (error instanceof Error ? error.message : '未知错误'))
    } finally {
      setRecoveryLoading(false)
    }
  }, [recoveryFiles, recoveryTotalSize])

  const handleCleanupSingle = useCallback(async (tempFilePath: string): Promise<void> => {
    const file = recoveryFiles.find(f => f.tempFilePath === tempFilePath)
    if (!file) return

    const sizeMB = (file.size / 1024 / 1024).toFixed(2)
    const date = new Date(file.createdAt).toLocaleString('zh-CN')
    const confirmMessage = `确认删除此临时录制文件？\n\n大小：${sizeMB} MB\n创建时间：${date}\n\n此操作不可撤销。`

    if (!window.confirm(confirmMessage)) return

    setRecoveryLoading(true)
    try {
      await window.recorderApi.cleanupRecoverableRecording(tempFilePath)
      await refreshRecoveryFiles()
    } catch (error) {
      window.alert('清理失败：' + (error instanceof Error ? error.message : '未知错误'))
    } finally {
      setRecoveryLoading(false)
    }
  }, [recoveryFiles, refreshRecoveryFiles])

  const onDataAvailable = useCallback(async (event: BlobEvent): Promise<void> => {
    if (event.data.size === 0 || !sessionRef.current) {
      return
    }

    pendingChunksRef.current.push(event.data)

    const now = Date.now()
    const totalSize = pendingChunksRef.current.reduce((sum, chunk) => sum + chunk.size, 0)
    const timeSinceLastFlush = now - lastFlushTimeRef.current

    if (totalSize >= FLUSH_SIZE_THRESHOLD || timeSinceLastFlush >= FLUSH_TIME_THRESHOLD) {
      await flushPendingChunks()
    }
  }, [flushPendingChunks])

  const handleRecordingError = useCallback(async (error: Error): Promise<void> => {
    recordingErrorRef.current = true
    clearFlushTimer()
    
    await window.recorderApi.hideRecordingIndicator()
    setStatus('error')
    setErrorMessage(error.message)

    if (sessionRef.current) {
      await window.recorderApi.cancelRecording()
      sessionRef.current = null
    }

    stopTracks(canvasStreamRef.current)
    stopTracks(streamRef.current)
    stopTracks(micStreamRef.current)
    streamRef.current = null
    canvasStreamRef.current = null
    micStreamRef.current = null
    if (audioContextRef.current) {
      void audioContextRef.current.close()
      audioContextRef.current = null
    }
  }, [clearFlushTimer])

  const handleRecordingStop = async (): Promise<void> => {
    try {
      clearFlushTimer()

      await flushPendingChunks()

      if (!recordingErrorRef.current) {
        setStatus('saving')
      }

      if (animationFrameRef.current) {
        cancelAnimationFrame(animationFrameRef.current)
        animationFrameRef.current = null
      }

      await window.recorderApi.hideRecordingIndicator()

      if (!sessionRef.current) {
        if (!recordingErrorRef.current) {
          setStatus('idle')
        }
        return
      }

      const result = await window.recorderApi.completeRecording()

      if (!result.canceled && result.filePath) {
        setSavedPath(result.filePath)
      }

      sessionRef.current = null

      if (!recordingErrorRef.current) {
        setStatus('idle')
      }
    } catch (error) {
      setStatus('error')
      setErrorMessage(error instanceof Error ? error.message : '保存录制文件时发生未知错误。')
      
      if (sessionRef.current) {
        await window.recorderApi.cancelRecording()
        sessionRef.current = null
      }
    } finally {
      recordingErrorRef.current = false
      pendingChunksRef.current = []
      chunksRef.current = []
      mediaRecorderRef.current = null
      stopTracks(streamRef.current)
      stopTracks(canvasStreamRef.current)
      stopTracks(micStreamRef.current)
      streamRef.current = null
      canvasStreamRef.current = null
      micStreamRef.current = null
      if (audioContextRef.current) {
        void audioContextRef.current.close()
        audioContextRef.current = null
      }
      if (captureVideoRef.current) {
        captureVideoRef.current.srcObject = null
      }
      setRegion(null)
    }
  }

  const drawCanvasFrame = useCallback((selectedRegion: Region) => {
    if (!canvasRef.current || !captureVideoRef.current) {
      return
    }

    const canvas = canvasRef.current
    const video = captureVideoRef.current
    const context = canvas.getContext('2d')

    if (!context) {
      return
    }

    context.drawImage(
      video,
      selectedRegion.x,
      selectedRegion.y,
      selectedRegion.width,
      selectedRegion.height,
      0,
      0,
      selectedRegion.width,
      selectedRegion.height
    )

    animationFrameRef.current = requestAnimationFrame(() => {
      drawCanvasFrame(selectedRegion)
    })
  }, [])

  const startCanvasRecording = useCallback(async (
    selection: RegionSelection,
    displayBounds: DisplayBounds
  ): Promise<void> => {
    if (!canvasRef.current || !captureVideoRef.current) {
      throw new Error('区域录制初始化失败。')
    }

    const canvas = canvasRef.current
    const video = captureVideoRef.current

    await waitForVideoReady(video)

    const normalizedRegion = mapSelectionToVideoRegion(
      selection,
      displayBounds,
      video.videoWidth,
      video.videoHeight
    )

    if (!normalizedRegion) {
      throw new Error('所选区域过小或超出录制范围，请重新框选。')
    }

    setRegion(normalizedRegion)
    canvas.width = normalizedRegion.width
    canvas.height = normalizedRegion.height

    const canvasStream = canvas.captureStream(params.frameRate)
    canvasStreamRef.current = canvasStream

    const audioTrack = await getMixedAudioTrack(streamRef.current, micStreamRef.current, audioContextRef)
    if (audioTrack) {
      canvasStream.addTrack(audioTrack)
    }

    const mimeType = getSupportedMimeType()
    const recorder = mimeType
      ? new MediaRecorder(canvasStream, { mimeType })
      : new MediaRecorder(canvasStream)

    recorder.ondataavailable = (event) => {
      void onDataAvailable(event)
    }

    recorder.onerror = async (event) => {
      await handleRecordingError(new Error(event.error?.message || '录制时发生未知错误。'))
    }

    recorder.onstop = () => {
      void handleRecordingStop()
    }

    mediaRecorderRef.current = recorder
    setStatus('recording')

    await window.recorderApi.showRecordingIndicator()
    await window.recorderApi.windowControl.minimize()

    lastFlushTimeRef.current = Date.now()
    flushTimerRef.current = window.setInterval(() => {
      void flushPendingChunks()
    }, FLUSH_TIME_THRESHOLD)

    recorder.start(1000)
    drawCanvasFrame(normalizedRegion)
  }, [drawCanvasFrame, handleRecordingStop, params.frameRate, onDataAvailable, handleRecordingError, flushPendingChunks])

  const startFullRecording = useCallback(async (stream: MediaStream) => {
    const videoTracks = stream.getVideoTracks()
    const audioTrack = await getMixedAudioTrack(stream, micStreamRef.current, audioContextRef)
    
    const tracks: MediaStreamTrack[] = [...videoTracks]
    if (audioTrack) {
      tracks.push(audioTrack)
    }
    
    const recordingStream = new MediaStream(tracks)
    
    const mimeType = getSupportedMimeType()
    const recorder = mimeType
      ? new MediaRecorder(recordingStream, { mimeType })
      : new MediaRecorder(recordingStream)

    recorder.ondataavailable = (event) => {
      void onDataAvailable(event)
    }

    recorder.onerror = async (event) => {
      await handleRecordingError(new Error(event.error?.message || '录制时发生未知错误。'))
    }

    recorder.onstop = () => {
      void handleRecordingStop()
    }

    const [videoTrack] = videoTracks
    if (videoTrack) {
      videoTrack.onended = () => {
        if (recorder.state !== 'inactive') {
          recorder.stop()
        }
      }
    }

    mediaRecorderRef.current = recorder
    setStatus('recording')

    await window.recorderApi.showRecordingIndicator()
    await window.recorderApi.windowControl.minimize()

    lastFlushTimeRef.current = Date.now()
    flushTimerRef.current = window.setInterval(() => {
      void flushPendingChunks()
    }, FLUSH_TIME_THRESHOLD)

    recorder.start(1000)
  }, [handleRecordingStop, onDataAvailable, handleRecordingError, flushPendingChunks])

  const startRecording = async (): Promise<void> => {
    try {
      recordingErrorRef.current = false
      setErrorMessage('')
      setSavedPath('')
      pendingChunksRef.current = []
      chunksRef.current = []
      lastFlushTimeRef.current = Date.now()
      sessionRef.current = null
      recordingStartTimeRef.current = Date.now()
      setRegion(null)
      setStatus('selecting')

      const session = await window.recorderApi.createRecording()
      sessionRef.current = session

      if (params.microphoneEnabled) {
        try {
          const micStream = await navigator.mediaDevices.getUserMedia({ audio: true })
          micStreamRef.current = micStream
        } catch (micError) {
          await window.recorderApi.cancelRecording()
          sessionRef.current = null
          throw new Error('无法访问麦克风，请检查权限设置。')
        }
      }

      const selectedResolution = resolutionOptions.find(o => o.name === params.resolution) || resolutionOptions[0]
      if (!selectedResolution) {
        await window.recorderApi.cancelRecording()
        sessionRef.current = null
        throw new Error('无法获取分辨率设置。')
      }

      const stream = await navigator.mediaDevices.getDisplayMedia({
        video: {
          frameRate: params.frameRate,
          width: { ideal: selectedResolution.width },
          height: { ideal: selectedResolution.height }
        },
        audio: params.audioEnabled
      })

      streamRef.current = stream

      if (captureVideoRef.current) {
        captureVideoRef.current.srcObject = stream
      }

      if (params.captureMode === 'region') {
        setStatus('region-select')

        if (captureVideoRef.current) {
          await waitForVideoReady(captureVideoRef.current)
        }

        let backgroundFrameDataUrl = ''
        if (captureVideoRef.current && captureVideoRef.current.videoWidth > 0) {
          const video = captureVideoRef.current
          const canvas = document.createElement('canvas')
          canvas.width = video.videoWidth
          canvas.height = video.videoHeight
          const ctx = canvas.getContext('2d')
          if (ctx) {
            ctx.drawImage(video, 0, 0, canvas.width, canvas.height)
            backgroundFrameDataUrl = canvas.toDataURL('image/png')
          }
        }

        const selectionResult = await window.recorderApi.selectRegion({ backgroundFrameDataUrl })

        if (selectionResult.canceled || !selectionResult.region || !selectionResult.displayBounds) {
          await window.recorderApi.cancelRecording()
          sessionRef.current = null
          stopTracks(stream)
          stopTracks(micStreamRef.current)
          streamRef.current = null
          micStreamRef.current = null
          if (captureVideoRef.current) {
            captureVideoRef.current.srcObject = null
          }
          setStatus('idle')
          return
        }

        await startCanvasRecording(selectionResult.region, selectionResult.displayBounds)
        return
      }

      await startFullRecording(stream)
    } catch (error) {
      await window.recorderApi.hideRecordingIndicator()
      
      if (sessionRef.current) {
        await window.recorderApi.cancelRecording()
        sessionRef.current = null
      }
      
      setStatus('error')
      setErrorMessage(error instanceof Error ? error.message : '无法启动录制。')
      stopTracks(streamRef.current)
      stopTracks(micStreamRef.current)
      streamRef.current = null
      micStreamRef.current = null
      if (captureVideoRef.current) {
        captureVideoRef.current.srcObject = null
      }
    }
  }

  const stopRecording = (): void => {
    if (!mediaRecorderRef.current || mediaRecorderRef.current.state === 'inactive') {
      return
    }

    mediaRecorderRef.current.stop()
  }

  useEffect(() => {
    if (status === 'idle') {
      void refreshRecoveryFiles()
    }
  }, [status, refreshRecoveryFiles])

  useEffect(() => {
    const checkRecovery = async () => {
      try {
        const recoveryInfo = await window.recorderApi.checkRecoverableRecording()
        if (recoveryInfo) {
          const sizeMB = (recoveryInfo.size / 1024 / 1024).toFixed(2)
          const date = new Date(recoveryInfo.createdAt).toLocaleString('zh-CN')
          const message = `检测到未完成的录制（${sizeMB}MB，创建于 ${date}）。\n\n是否尝试恢复并保存？`
          const shouldRecover = window.confirm(message)

          if (shouldRecover) {
            try {
              const result = await window.recorderApi.recoverRecording(recoveryInfo.tempFilePath)
              if (!result.canceled && result.filePath) {
                setSavedPath(result.filePath)
              }
            } catch (error) {
              window.alert('恢复失败：' + (error instanceof Error ? error.message : '未知错误'))
              await window.recorderApi.deleteRecoverableRecording(recoveryInfo.tempFilePath)
            }
          } else {
            await window.recorderApi.deleteRecoverableRecording(recoveryInfo.tempFilePath)
          }
        }
      } catch {}
    }

    checkRecovery()

    return () => {
      if (animationFrameRef.current) {
        cancelAnimationFrame(animationFrameRef.current)
      }
      clearFlushTimer()
      void window.recorderApi.cancelRegionSelection()
      void window.recorderApi.hideRecordingIndicator()
      stopTracks(streamRef.current)
      stopTracks(canvasStreamRef.current)
      stopTracks(micStreamRef.current)
      if (audioContextRef.current) {
        void audioContextRef.current.close()
      }
    }
  }, [clearFlushTimer])

  return (
    <main className="app-shell">
      <section className="hero-card">
        <span className={`status-chip status-${status}`}>{statusText}</span>
        <h1>桌面录屏</h1>
        <p className="subtitle">
          配置录制参数，选择屏幕源，开始录制并导出 MP4 文件。支持全屏录制和直接屏幕框选区域录制。
        </p>

        {status === 'idle' && (
          <div className="params-panel">
            <h3>录制参数</h3>
            <div className="params-grid">
              <div className="param-field">
                <label>分辨率</label>
                <select
                  value={params.resolution}
                  onChange={(event) =>
                    setParams((current) => ({
                      ...current,
                      resolution: event.target.value
                    }))
                  }
                >
                  {resolutionOptions.map(option => (
                    <option key={option.name} value={option.name}>
                      {option.name} ({option.width}x{option.height})
                    </option>
                  ))}
                </select>
              </div>

              <div className="param-field">
                <label>帧率</label>
                <select
                  value={params.frameRate}
                  onChange={(event) =>
                    setParams((current) => ({
                      ...current,
                      frameRate: parseInt(event.target.value)
                    }))
                  }
                >
                  <option value={60}>60 FPS</option>
                  <option value={30}>30 FPS</option>
                  <option value={24}>24 FPS</option>
                  <option value={15}>15 FPS</option>
                </select>
              </div>

              <div className="param-field">
                <label>系统音频</label>
                <button
                  className={`toggle-btn ${params.audioEnabled ? 'active' : ''}`}
                  onClick={() =>
                    setParams((current) => ({
                      ...current,
                      audioEnabled: !current.audioEnabled
                    }))
                  }
                >
                  {params.audioEnabled ? '开启' : '关闭'}
                </button>
              </div>

              <div className="param-field">
                <label>麦克风</label>
                <button
                  className={`toggle-btn ${params.microphoneEnabled ? 'active' : ''}`}
                  onClick={() =>
                    setParams((current) => ({
                      ...current,
                      microphoneEnabled: !current.microphoneEnabled
                    }))
                  }
                >
                  {params.microphoneEnabled ? '开启' : '关闭'}
                </button>
              </div>

              <div className="param-field">
                <label>录制模式</label>
                <div className="mode-toggle">
                  <button
                    className={`mode-btn ${params.captureMode === 'full' ? 'active' : ''}`}
                    onClick={() =>
                      setParams((current) => ({
                        ...current,
                        captureMode: 'full'
                      }))
                    }
                  >
                    全屏
                  </button>
                  <button
                    className={`mode-btn ${params.captureMode === 'region' ? 'active' : ''}`}
                    onClick={() =>
                      setParams((current) => ({
                        ...current,
                        captureMode: 'region'
                      }))
                    }
                  >
                    区域
                  </button>
                </div>
              </div>
            </div>
          </div>
        )}

        <video ref={captureVideoRef} autoPlay muted playsInline style={{ display: 'none' }} />
        <canvas ref={canvasRef} style={{ display: 'none' }} />

        <div className="actions">
          <button
            className="primary"
            onClick={() => void startRecording()}
            disabled={status === 'recording' || status === 'saving' || status === 'selecting' || status === 'region-select'}
          >
            开始录制
          </button>
          <button
            className="secondary"
            onClick={stopRecording}
            disabled={status !== 'recording'}
          >
            停止录制
          </button>
        </div>

        {status === 'idle' && (
          <div className="panel-grid">
            <article className="panel">
              <h2>当前能力</h2>
              <ul>
                <li>Electron + React + TypeScript 项目骨架</li>
                <li>可配置分辨率、帧率和音频设置</li>
                <li>支持全屏录制和直接框选区域录制</li>
                <li>通过 preload + IPC 自动导出 MP4</li>
                <li>流式保存，内存占用低于 10MB</li>
                <li>录制崩溃后可恢复临时文件</li>
              </ul>
            </article>

            <article className="panel">
              <h2>运行说明</h2>
              <ul>
                <li>配置录制参数后点击开始录制</li>
                <li>区域模式下会直接进入屏幕框选</li>
                <li>结束录制后会弹出 MP4 保存对话框</li>
                <li>临时文件实时保存，录制中断可恢复</li>
              </ul>
            </article>

            <article className="panel">
              <h2>临时文件管理</h2>
              {recoveryLoading ? (
                <p>正在加载...</p>
              ) : recoveryFiles.length === 0 ? (
                <p>没有残留的临时录制文件。</p>
              ) : (
                <>
                  <div className="recovery-summary">
                    <span>文件数量：{recoveryFiles.length}</span>
                    <span>总大小：{(recoveryTotalSize / 1024 / 1024).toFixed(2)} MB</span>
                  </div>

                  <ul className="recovery-list">
                    {recoveryFiles.map((file) => (
                      <li key={file.tempFilePath} className="recovery-item">
                        <div className="recovery-info">
                          <span>{(file.size / 1024 / 1024).toFixed(2)} MB</span>
                          <span>{new Date(file.createdAt).toLocaleString('zh-CN')}</span>
                        </div>
                        <button
                          className="recovery-delete-btn"
                          onClick={() => void handleCleanupSingle(file.tempFilePath)}
                          disabled={recoveryLoading}
                        >
                          删除
                        </button>
                      </li>
                    ))}
                  </ul>

                  <button
                    className="recovery-cleanup-all-btn"
                    onClick={() => void handleCleanupAll()}
                    disabled={recoveryLoading}
                  >
                    清理所有临时文件
                  </button>
                </>
              )}
            </article>
          </div>
        )}

        {savedPath ? <p className="success">最近一次保存位置：{savedPath}</p> : null}
        {errorMessage ? <p className="error">错误信息：{errorMessage}</p> : null}
      </section>
    </main>
  )
}

export default App
