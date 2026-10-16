import 'dockview-core/dist/styles/dockview.css'
import './style.css'
import { startApp } from './app'

// Core's address: ?core=ws://127.0.0.1:9000 overrides the default.
const params = new URLSearchParams(location.search)
startApp(document.querySelector<HTMLDivElement>('#app')!, params.get('core') ?? 'ws://127.0.0.1:8787')
