import { useCallback, useEffect, useMemo, useState } from 'react'

type Point = {
  x: number
  y: number
}

type SelectionRect = {
  x: number
  y: number
  width: number
  height: number
}

const MIN_REGION_SIZE = 50

function RegionSelectionOverlay() {
  const [selectionStart, setSelectionStart] = useState<Point | null>(null)
  const [selectionCurrent, setSelectionCurrent] = useState<Point | null>(null)
  const [isDragging, setIsDragging] = useState(false)
  const [isMoving, setIsMoving] = useState(false)
  const [moveOffset, setMoveOffset] = useState<Point | null>(null)
  const [isSubmitting, setIsSubmitting] = useState(false)
  const [hintText, setHintText] = useState('拖动鼠标框选录制区域，按 Esc 取消')
  const [backgroundFrame, setBackgroundFrame] = useState<string | null>(null)

  const selectionRect = useMemo<SelectionRect | null>(() => {
    if (!selectionStart || !selectionCurrent) {
      return null
    }

    return {
      x: Math.min(selectionStart.x, selectionCurrent.x),
      y: Math.min(selectionStart.y, selectionCurrent.y),
      width: Math.abs(selectionCurrent.x - selectionStart.x),
      height: Math.abs(selectionCurrent.y - selectionStart.y)
    }
  }, [selectionCurrent, selectionStart])

  const hasValidSelection = useMemo(() => {
    return selectionRect !== null &&
      selectionRect.width >= MIN_REGION_SIZE &&
      selectionRect.height >= MIN_REGION_SIZE
  }, [selectionRect])

  const handleCancel = useCallback(() => {
    void window.recorderApi.cancelRegionSelection()
  }, [])

  const handleResetSelection = useCallback(() => {
    setSelectionStart(null)
    setSelectionCurrent(null)
    setIsDragging(false)
    setIsMoving(false)
    setMoveOffset(null)
    setHintText('拖动鼠标框选录制区域，按 Esc 取消')
  }, [])

  const handleConfirm = useCallback(async () => {
    if (!selectionRect || isSubmitting) {
      return
    }

    setIsSubmitting(true)

    try {
      await window.recorderApi.submitRegionSelection(selectionRect)
    } finally {
      setIsSubmitting(false)
    }
  }, [isSubmitting, selectionRect])

  const stopOverlayActionPropagation = useCallback((event: React.MouseEvent<HTMLElement>) => {
    event.stopPropagation()
  }, [])

  const handleMouseDown = useCallback((event: React.MouseEvent<HTMLDivElement>) => {
    if (event.button !== 0 || isSubmitting || isMoving) {
      return
    }

    // Start new selection, replacing any existing one
    setIsDragging(true)
    setHintText('松开鼠标完成框选')
    setSelectionStart({ x: event.clientX, y: event.clientY })
    setSelectionCurrent({ x: event.clientX, y: event.clientY })
  }, [isSubmitting, isMoving])

  const handleMouseMove = useCallback((event: React.MouseEvent<HTMLDivElement>) => {
    if (isSubmitting) {
      return
    }

    if (isMoving && moveOffset && selectionRect) {
      // Moving existing selection
      const newX = event.clientX - moveOffset.x
      const newY = event.clientY - moveOffset.y

      // Clamp to viewport bounds
      const clampedX = Math.max(0, Math.min(newX, window.innerWidth - selectionRect.width))
      const clampedY = Math.max(0, Math.min(newY, window.innerHeight - selectionRect.height))

      setSelectionStart({ x: clampedX, y: clampedY })
      setSelectionCurrent({ x: clampedX + selectionRect.width, y: clampedY + selectionRect.height })
      return
    }

    if (!selectionStart || !isDragging) {
      return
    }

    setSelectionCurrent({ x: event.clientX, y: event.clientY })
  }, [isDragging, isMoving, isSubmitting, moveOffset, selectionRect, selectionStart])

  const handleMouseUp = useCallback(() => {
    if (isSubmitting) {
      return
    }

    if (isMoving) {
      setIsMoving(false)
      setMoveOffset(null)
      setHintText('点击确认开始录制，拖动选区可移动位置，或重新拖动调整选区')
      return
    }

    if (!isDragging) {
      return
    }

    setIsDragging(false)

    if (!selectionRect) {
      return
    }

    if (selectionRect.width < MIN_REGION_SIZE || selectionRect.height < MIN_REGION_SIZE) {
      setSelectionStart(null)
      setSelectionCurrent(null)
      setHintText(`选区至少需要 ${MIN_REGION_SIZE} x ${MIN_REGION_SIZE} 像素，请重新框选`)
      return
    }

    setHintText('点击确认开始录制，拖动选区可移动位置，或重新拖动调整选区')
  }, [isDragging, isMoving, isSubmitting, selectionRect])

  const handleSelectionMouseDown = useCallback((event: React.MouseEvent<HTMLDivElement>) => {
    if (event.button !== 0 || isSubmitting || !selectionRect) {
      return
    }

    event.stopPropagation()
    setIsMoving(true)
    setMoveOffset({ x: event.clientX - selectionRect.x, y: event.clientY - selectionRect.y })
    setHintText('拖动以移动选区位置')
  }, [isSubmitting, selectionRect])

  const handlePointerExit = useCallback(() => {
    if (isMoving) {
      setIsMoving(false)
      setMoveOffset(null)
      setHintText('点击确认开始录制，拖动选区可移动位置，或重新拖动调整选区')
      return
    }

    if (isDragging) {
      setIsDragging(false)

      if (!selectionRect) {
        return
      }

      if (selectionRect.width < MIN_REGION_SIZE || selectionRect.height < MIN_REGION_SIZE) {
        setSelectionStart(null)
        setSelectionCurrent(null)
        setHintText(`选区至少需要 ${MIN_REGION_SIZE} x ${MIN_REGION_SIZE} 像素，请重新框选`)
        return
      }

      setHintText('点击确认开始录制，拖动选区可移动位置，或重新拖动调整选区')
    }
  }, [isDragging, isMoving, selectionRect])

  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent): void => {
      if (event.key === 'Escape') {
        handleCancel()
      }
    }

    window.addEventListener('keydown', handleKeyDown)

    return () => {
      window.removeEventListener('keydown', handleKeyDown)
    }
  }, [handleCancel])

  useEffect(() => {
    void window.recorderApi.getRegionSelectionBackgroundFrame().then((dataUrl) => {
      setBackgroundFrame(dataUrl)
    })
  }, [])

  return (
    <main
      className="overlay-shell"
      onMouseDown={handleMouseDown}
      onMouseMove={handleMouseMove}
      onMouseUp={handleMouseUp}
      onMouseLeave={handlePointerExit}
    >
      {backgroundFrame && (
        <img
          className="overlay-background"
          src={backgroundFrame}
          alt=""
          draggable={false}
        />
      )}
      <div className="overlay-hint">{hintText}</div>
      {selectionRect && (
        <div
          className="overlay-selection"
          style={{
            left: selectionRect.x,
            top: selectionRect.y,
            width: selectionRect.width,
            height: selectionRect.height
          }}
          onMouseDown={handleSelectionMouseDown}
        >
          <span className="overlay-selection-label">
            {Math.round(selectionRect.width)} × {Math.round(selectionRect.height)}
          </span>
        </div>
      )}
      {hasValidSelection && !isDragging && (
        <div
          className="overlay-actions"
          onMouseDown={stopOverlayActionPropagation}
          onClick={stopOverlayActionPropagation}
        >
          <button
            className="overlay-btn overlay-btn-confirm"
            onClick={handleConfirm}
            disabled={isSubmitting}
          >
            {isSubmitting ? '提交中...' : '确认'}
          </button>
          <button
            className="overlay-btn overlay-btn-reset"
            onClick={handleResetSelection}
            disabled={isSubmitting}
          >
            重新选择
          </button>
          <button
            className="overlay-btn overlay-btn-cancel"
            onClick={handleCancel}
            disabled={isSubmitting}
          >
            取消
          </button>
        </div>
      )}
    </main>
  )
}

export default RegionSelectionOverlay
