import { useEffect, useState } from 'react'

function formatDuration(ms: number): string {
  const totalSeconds = Math.floor(ms / 1000)
  const hours = Math.floor(totalSeconds / 3600)
  const minutes = Math.floor((totalSeconds % 3600) / 60)
  const seconds = totalSeconds % 60

  if (hours > 0) {
    return `${hours}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`
  }

  return `${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`
}

function RecordingIndicatorOverlay() {
  const params = new URLSearchParams(window.location.search)
  const startTime = Number(params.get('startTime')) || Date.now()

  const [duration, setDuration] = useState(0)

  useEffect(() => {
    const updateDuration = (): void => {
      setDuration(Date.now() - startTime)
    }

    updateDuration()

    const intervalId = setInterval(updateDuration, 1000)

    return () => {
      clearInterval(intervalId)
    }
  }, [startTime])

  return (
    <div className="recording-indicator-shell">
      <div className="recording-indicator-content">
        <span className="recording-indicator-dot" />
        <span className="recording-indicator-text">正在录制</span>
        <span className="recording-indicator-time">{formatDuration(duration)}</span>
      </div>
    </div>
  )
}

export default RecordingIndicatorOverlay