import React from 'react'
import ReactDOM from 'react-dom/client'
import App from './App'
import RegionSelectionOverlay from './RegionSelectionOverlay'
import RecordingIndicatorOverlay from './RecordingIndicatorOverlay'
import './styles.css'

const overlayParam = new URLSearchParams(window.location.search).get('overlay')
const isRegionSelectionOverlay = overlayParam === 'region-selection'
const isRecordingIndicator = overlayParam === 'recording-indicator'

function getRootComponent() {
  if (isRegionSelectionOverlay) {
    return <RegionSelectionOverlay />
  }
  if (isRecordingIndicator) {
    return <RecordingIndicatorOverlay />
  }
  return <App />
}

ReactDOM.createRoot(document.getElementById('root') as HTMLElement).render(
  <React.StrictMode>
    {getRootComponent()}
  </React.StrictMode>
)
