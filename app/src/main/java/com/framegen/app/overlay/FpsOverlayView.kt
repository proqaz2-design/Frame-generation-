package com.framegen.app.overlay

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.*
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.*
import android.widget.FrameLayout
import com.framegen.app.engine.FrameGenEngine
import com.framegen.app.service.FrameGenService

/**
 * FpsOverlayView — In-game FPS counter overlay.
 *
 * Shows real-time stats floating over any game, similar to
 * Steam/RTSS/PUBG FPS counters. Draggable, semi-transparent.
 *
 * Displays:
 * - Current output FPS (e.g. 60)
 * - Original input FPS (e.g. 30)
 * - Frame time graph (like RTSS)
 * - Gen status indicator
 * - GPU temperature
 * - FPS drop alerts
 */
@SuppressLint("ViewConstructor")
class FpsOverlayView(context: Context) : FrameLayout(context) {

    companion object {
        private const val TAG = "FpsOverlay"
        private const val GRAPH_POINTS = 120       // 2 seconds of history at 60fps
        private const val UPDATE_INTERVAL_MS = 16L // ~60 updates/sec
        private const val FPS_DROP_THRESHOLD = 0.8f // Alert if drops below 80% of target
    }

    // Paint objects
    private val bgPaint = Paint().apply {
        color = Color.argb(180, 10, 10, 26)
        style = Paint.Style.FILL
    }
    private val borderPaint = Paint().apply {
        color = Color.argb(200, 0, 212, 255)
        style = Paint.Style.STROKE
        strokeWidth = 2f
    }
    private val fpsPaint = Paint().apply {
        color = Color.rgb(0, 255, 136)
        textSize = 42f
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
        isAntiAlias = true
    }
    private val labelPaint = Paint().apply {
        color = Color.rgb(136, 153, 170)
        textSize = 20f
        typeface = Typeface.MONOSPACE
        isAntiAlias = true
    }
    private val valuePaint = Paint().apply {
        color = Color.rgb(204, 220, 240)
        textSize = 22f
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
        isAntiAlias = true
    }
    private val graphPaint = Paint().apply {
        color = Color.rgb(0, 212, 255)
        style = Paint.Style.STROKE
        strokeWidth = 1.5f
        isAntiAlias = true
    }
    private val graphFillPaint = Paint().apply {
        style = Paint.Style.FILL
        isAntiAlias = true
    }
    private val dropPaint = Paint().apply {
        color = Color.rgb(230, 57, 70)
        textSize = 20f
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
        isAntiAlias = true
    }
    private val genPaint = Paint().apply {
        color = Color.rgb(0, 212, 255)
        textSize = 20f
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
        isAntiAlias = true
    }

    // Data
    private val fpsHistory = FloatArray(GRAPH_POINTS)
    private val frameTimeHistory = FloatArray(GRAPH_POINTS)
    private var historyIndex = 0
    private var historyCount = 0

    private var currentOutputFps = 0f
    private var currentInputFps = 0f
    private var currentFrameTimeMs = 0f
    private var gpuTemp = 0f
    private var isGenerating = false
    private var isThrottled = false
    private var targetFps = 60f
    private var framesGenerated = 0L
    private var framesDropped = 0L

    // FPS drop tracking
    private var fpsDropCount = 0
    private var lastDropTime = 0L
    private var minFps = Float.MAX_VALUE
    private var maxFps = 0f
    private var avgFps = 0f
    private var fpsSum = 0f
    private var fpsSamples = 0

    // Layout
    private var overlayWidth = 320
    private var overlayHeight = 280
    private var isExpanded = true
    private var isDragging = false
    private var dragStartX = 0f
    private var dragStartY = 0f
    private var viewStartX = 0f
    private var viewStartY = 0f

    private val handler = Handler(Looper.getMainLooper())
    private var engine: FrameGenEngine? = null
    private var isRunning = false

    private val graphPath = Path()
    private val graphFillPath = Path()
    private val rect = RectF()

    init {
        setWillNotDraw(false)
        isClickable = true
        isFocusable = true

        // Default position: top-left
        x = 20f
        y = 80f
    }

    fun attachEngine(engine: FrameGenEngine) {
        this.engine = engine
    }

    fun start() {
        if (isRunning) return
        isRunning = true
        resetStats()
        scheduleUpdate()
        Log.i(TAG, "FPS overlay started")
    }

    fun stop() {
        isRunning = false
        handler.removeCallbacksAndMessages(null)
        Log.i(TAG, "FPS overlay stopped")
    }

    fun setTargetFps(fps: Float) {
        targetFps = fps
    }

    private fun resetStats() {
        fpsDropCount = 0
        minFps = Float.MAX_VALUE
        maxFps = 0f
        avgFps = 0f
        fpsSum = 0f
        fpsSamples = 0
        historyCount = 0
        historyIndex = 0
    }

    private fun scheduleUpdate() {
        if (!isRunning) return
        handler.postDelayed({
            updateData()
            invalidate()
            scheduleUpdate()
        }, UPDATE_INTERVAL_MS)
    }

    private fun updateData() {
        val eng = engine ?: return

        try {
            val stats = eng.getStats()

            currentOutputFps = stats.effectiveFps
            currentFrameTimeMs = stats.totalMs

            // Calculate input FPS from output and generated ratio
            isGenerating = FrameGenService.isActivelyGenerating
            if (isGenerating && stats.effectiveFps > 0) {
                // Input FPS = output / multiplier
                val mode = when {
                    targetFps >= 120 -> 4f   // 30→120 = 4x
                    targetFps >= 90 -> 3f    // 30→90 = 3x
                    else -> 2f               // 30→60 = 2x
                }
                currentInputFps = stats.effectiveFps / mode
            } else {
                currentInputFps = stats.effectiveFps
            }

            gpuTemp = eng.getGpuTemperature()
            isThrottled = eng.isThermalThrottled()
            framesGenerated = stats.framesGenerated
            framesDropped = stats.framesDropped

            // Update history
            fpsHistory[historyIndex] = currentOutputFps
            frameTimeHistory[historyIndex] = currentFrameTimeMs
            historyIndex = (historyIndex + 1) % GRAPH_POINTS
            if (historyCount < GRAPH_POINTS) historyCount++

            // Track stats
            if (currentOutputFps > 0) {
                if (currentOutputFps < minFps) minFps = currentOutputFps
                if (currentOutputFps > maxFps) maxFps = currentOutputFps
                fpsSum += currentOutputFps
                fpsSamples++
                avgFps = fpsSum / fpsSamples

                // Detect FPS drops
                val dropThreshold = targetFps * FPS_DROP_THRESHOLD
                if (currentOutputFps < dropThreshold) {
                    val now = System.currentTimeMillis()
                    if (now - lastDropTime > 1000) { // Don't count rapid consecutive drops
                        fpsDropCount++
                        lastDropTime = now
                    }
                }
            }

        } catch (e: Exception) {
            // Engine might not be ready
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val w = if (isExpanded) overlayWidth.toFloat() else 120f
        val h = if (isExpanded) overlayHeight.toFloat() else 48f

        // Background
        rect.set(0f, 0f, w, h)
        canvas.drawRoundRect(rect, 8f, 8f, bgPaint)
        canvas.drawRoundRect(rect, 8f, 8f, borderPaint)

        if (!isExpanded) {
            // Compact: just FPS number
            drawCompactMode(canvas)
            return
        }

        var y = 0f

        // ── Header: Big FPS number ──
        y += 38f
        val fpsText = "%.0f".format(currentOutputFps)
        canvas.drawText(fpsText, 12f, y, fpsPaint)

        // "FPS" label next to number
        val fpsWidth = fpsPaint.measureText(fpsText)
        canvas.drawText("FPS", 12f + fpsWidth + 6f, y, labelPaint)

        // Gen indicator
        if (isGenerating) {
            canvas.drawText("⚡GEN", w - 80f, y - 12f, genPaint)
        }

        // ── Input → Output line ──
        y += 22f
        val inputOutputText = "%.0f → %.0f fps".format(currentInputFps, currentOutputFps)
        canvas.drawText(inputOutputText, 12f, y, valuePaint)

        // Multiplier badge
        if (isGenerating && currentInputFps > 0) {
            val mult = "%.1fx".format(currentOutputFps / currentInputFps)
            val multPaint = Paint(genPaint).apply { textSize = 18f }
            canvas.drawText(mult, w - 60f, y, multPaint)
        }

        // ── Frame time ──
        y += 20f
        val ftText = "%.1f ms".format(currentFrameTimeMs)
        canvas.drawText("ft: ", 12f, y, labelPaint)
        canvas.drawText(ftText, 42f, y, valuePaint)

        // GPU temp
        val tempColor = when {
            gpuTemp >= 80 -> Color.rgb(230, 57, 70)
            gpuTemp >= 70 -> Color.rgb(255, 183, 0)
            else -> Color.rgb(0, 255, 136)
        }
        val tempPaint = Paint(valuePaint).apply { color = tempColor; textSize = 20f }
        canvas.drawText("%.0f°C".format(gpuTemp), w - 70f, y, tempPaint)
        if (isThrottled) {
            canvas.drawText("⚠", w - 90f, y, dropPaint)
        }

        // ── FPS Graph ──
        y += 8f
        val graphLeft = 12f
        val graphTop = y
        val graphRight = w - 12f
        val graphBottom = y + 80f
        val graphH = graphBottom - graphTop
        val graphW = graphRight - graphLeft

        // Graph background
        val graphBgPaint = Paint().apply {
            color = Color.argb(60, 0, 0, 0)
            style = Paint.Style.FILL
        }
        rect.set(graphLeft, graphTop, graphRight, graphBottom)
        canvas.drawRect(rect, graphBgPaint)

        // Target FPS line
        val targetLinePaint = Paint().apply {
            color = Color.argb(100, 0, 255, 136)
            style = Paint.Style.STROKE
            strokeWidth = 1f
            pathEffect = DashPathEffect(floatArrayOf(4f, 4f), 0f)
        }
        val maxGraphFps = targetFps * 1.2f
        val targetY = graphBottom - (targetFps / maxGraphFps) * graphH
        canvas.drawLine(graphLeft, targetY, graphRight, targetY, targetLinePaint)

        // Draw FPS graph
        if (historyCount > 1) {
            graphPath.reset()
            graphFillPath.reset()

            val step = graphW / (GRAPH_POINTS - 1).toFloat()
            var firstPoint = true

            for (i in 0 until historyCount) {
                val idx = (historyIndex - historyCount + i + GRAPH_POINTS) % GRAPH_POINTS
                val fps = fpsHistory[idx]
                val px = graphLeft + i * step
                val py = graphBottom - (fps / maxGraphFps).coerceIn(0f, 1f) * graphH

                if (firstPoint) {
                    graphPath.moveTo(px, py)
                    graphFillPath.moveTo(px, graphBottom)
                    graphFillPath.lineTo(px, py)
                    firstPoint = false
                } else {
                    graphPath.lineTo(px, py)
                    graphFillPath.lineTo(px, py)
                }
            }

            // Fill under the curve
            val lastX = graphLeft + (historyCount - 1) * step
            graphFillPath.lineTo(lastX, graphBottom)
            graphFillPath.close()

            graphFillPaint.shader = LinearGradient(
                0f, graphTop, 0f, graphBottom,
                Color.argb(60, 0, 212, 255),
                Color.argb(10, 0, 212, 255),
                Shader.TileMode.CLAMP
            )
            canvas.drawPath(graphFillPath, graphFillPaint)

            // Draw line with color based on FPS
            val lineColor = when {
                currentOutputFps >= targetFps * 0.9f -> Color.rgb(0, 255, 136)
                currentOutputFps >= targetFps * 0.7f -> Color.rgb(255, 183, 0)
                else -> Color.rgb(230, 57, 70)
            }
            graphPaint.color = lineColor
            canvas.drawPath(graphPath, graphPaint)
        }

        // ── Stats row ──
        y = graphBottom + 18f
        val statsSmallPaint = Paint(labelPaint).apply { textSize = 16f }
        val statsValuePaint = Paint(valuePaint).apply { textSize = 16f }

        // Min/Avg/Max
        canvas.drawText("min:", 12f, y, statsSmallPaint)
        val minColor = if (minFps < targetFps * FPS_DROP_THRESHOLD) Color.rgb(230, 57, 70) else Color.rgb(204, 220, 240)
        statsValuePaint.color = minColor
        canvas.drawText("%.0f".format(if (minFps == Float.MAX_VALUE) 0f else minFps), 48f, y, statsValuePaint)

        canvas.drawText("avg:", 95f, y, statsSmallPaint)
        statsValuePaint.color = Color.rgb(204, 220, 240)
        canvas.drawText("%.0f".format(avgFps), 133f, y, statsValuePaint)

        canvas.drawText("max:", 180f, y, statsSmallPaint)
        canvas.drawText("%.0f".format(maxFps), 218f, y, statsValuePaint)

        // ── Drops counter ──
        y += 20f
        canvas.drawText("drops:", 12f, y, statsSmallPaint)
        dropPaint.textSize = 16f
        canvas.drawText("$fpsDropCount", 68f, y, if (fpsDropCount > 0) dropPaint else statsValuePaint)

        // Generated / dropped frames
        canvas.drawText("gen:", 120f, y, statsSmallPaint)
        canvas.drawText("$framesGenerated", 155f, y, statsValuePaint)

        if (framesDropped > 0) {
            canvas.drawText("skip:", 220f, y, statsSmallPaint)
            canvas.drawText("$framesDropped", 262f, y, dropPaint)
        }
    }

    private fun drawCompactMode(canvas: Canvas) {
        // Just show: "60 FPS ⚡"
        val fpsText = "%.0f".format(currentOutputFps)
        val compactFpsPaint = Paint(fpsPaint).apply { textSize = 30f }
        canvas.drawText(fpsText, 8f, 34f, compactFpsPaint)

        val labelX = compactFpsPaint.measureText(fpsText) + 12f
        canvas.drawText("FPS", labelX, 34f, labelPaint)

        if (isGenerating) {
            canvas.drawText("⚡", 96f, 34f, genPaint)
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                isDragging = false
                dragStartX = event.rawX
                dragStartY = event.rawY
                viewStartX = x
                viewStartY = y
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.rawX - dragStartX
                val dy = event.rawY - dragStartY
                if (dx * dx + dy * dy > 100) { // Moved more than 10px
                    isDragging = true
                    x = viewStartX + dx
                    y = viewStartY + dy
                }
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (!isDragging) {
                    // Tap: toggle expanded/compact
                    isExpanded = !isExpanded
                    val lp = layoutParams
                    if (lp != null) {
                        lp.width = if (isExpanded) overlayWidth else 120
                        lp.height = if (isExpanded) overlayHeight else 48
                        layoutParams = lp
                    }
                    invalidate()
                }
                isDragging = false
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = if (isExpanded) overlayWidth else 120
        val h = if (isExpanded) overlayHeight else 48
        setMeasuredDimension(w, h)
    }
}
