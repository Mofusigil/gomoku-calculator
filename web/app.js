(function () {
  "use strict";

  const BOARD_SIZE = 15;
  const EMPTY = 0;
  const BLACK = 1;
  const WHITE = 2;
  const ANALYSIS_CACHE_LIMIT = 64;
  const ANALYSIS_RESTART_DEBOUNCE_MS = 140;
  const ANALYSIS_START_TIMEOUT_MS = 8000;
  const ANALYSIS_POLL_TIMEOUT_MS = 15000;
  const ANALYSIS_STOP_TIMEOUT_MS = 2200;
  const ANALYSIS_STOP_ATTEMPTS = 3;
  const MODE_SWITCH_DEBOUNCE_MS = 80;
  const SCORE_LOGISTIC_BOUND = 17500;
  const SCORE_LOGISTIC_SCALE = 4500;
  const COLUMN_LABELS = "ABCDEFGHJKLMNOP";

  const elements = {
    canvas: document.getElementById("boardCanvas"),
    boardFrame: document.getElementById("boardFrame"),
    stoneCount: document.getElementById("stoneCount"),
    cursorCoordinate: document.getElementById("cursorCoordinate"),
    lastMoveText: document.getElementById("lastMoveText"),
    positionHash: document.getElementById("positionHash"),
    undoButton: document.getElementById("undoButton"),
    redoButton: document.getElementById("redoButton"),
    clearButton: document.getElementById("clearButton"),
    showMoveNumbersInput: document.getElementById("showMoveNumbersInput"),
    stoneToolControl: document.getElementById("stoneToolControl"),
    modeTabs: document.getElementById("modeTabs"),
    settingsHeading: document.getElementById("settingsHeading"),
    analysisSettings: document.getElementById("analysisSettings"),
    gameSettings: document.getElementById("gameSettings"),
    ruleControl: document.getElementById("ruleControl"),
    gameRuleControl: document.getElementById("gameRuleControl"),
    sideControl: document.getElementById("sideControl"),
    gameSideControl: document.getElementById("gameSideControl"),
    autoTurnInput: document.getElementById("autoTurnInput"),
    infiniteAnalysisInput: document.getElementById("infiniteAnalysisInput"),
    timeLimitInput: document.getElementById("timeLimitInput"),
    maxDepthInput: document.getElementById("maxDepthInput"),
    threatDepthInput: document.getElementById("threatDepthInput"),
    analyzeButton: document.getElementById("analyzeButton"),
    stopButton: document.getElementById("stopButton"),
    blackPlayerControl: document.getElementById("blackPlayerControl"),
    whitePlayerControl: document.getElementById("whitePlayerControl"),
    blackInitialInput: document.getElementById("blackInitialInput"),
    blackIncrementInput: document.getElementById("blackIncrementInput"),
    whiteInitialInput: document.getElementById("whiteInitialInput"),
    whiteIncrementInput: document.getElementById("whiteIncrementInput"),
    startGameButton: document.getElementById("startGameButton"),
    pauseGameButton: document.getElementById("pauseGameButton"),
    endGameButton: document.getElementById("endGameButton"),
    connectionStatus: document.getElementById("connectionStatus"),
    analysisResults: document.getElementById("analysisResults"),
    gameResultContent: document.getElementById("gameResultContent"),
    resultsHeading: document.getElementById("resultsHeading"),
    emptyState: document.getElementById("emptyState"),
    loadingState: document.getElementById("loadingState"),
    loadingDetail: document.getElementById("loadingDetail"),
    errorState: document.getElementById("errorState"),
    errorMessage: document.getElementById("errorMessage"),
    resultContent: document.getElementById("resultContent"),
    resultSubtitle: document.getElementById("resultSubtitle"),
    resultKind: document.getElementById("resultKind"),
    verdictBlock: document.getElementById("verdictBlock"),
    verdictLabel: document.getElementById("verdictLabel"),
    verdictValue: document.getElementById("verdictValue"),
    verdictNote: document.getElementById("verdictNote"),
    bestMoveButton: document.getElementById("bestMoveButton"),
    winrateBlock: document.getElementById("winrateBlock"),
    blackWinRate: document.getElementById("blackWinRate"),
    whiteWinRate: document.getElementById("whiteWinRate"),
    blackWinBar: document.getElementById("blackWinBar"),
    whiteWinBar: document.getElementById("whiteWinBar"),
    pvTitle: document.getElementById("pvTitle"),
    backToMainPvButton: document.getElementById("backToMainPvButton"),
    pvDepth: document.getElementById("pvDepth"),
    pvLine: document.getElementById("pvLine"),
    candidateCount: document.getElementById("candidateCount"),
    candidateList: document.getElementById("candidateList"),
    statNodes: document.getElementById("statNodes"),
    statDepth: document.getElementById("statDepth"),
    statTime: document.getElementById("statTime"),
    statCutoffs: document.getElementById("statCutoffs"),
    statTtHits: document.getElementById("statTtHits"),
    statNps: document.getElementById("statNps"),
    blackClockBox: document.getElementById("blackClockBox"),
    whiteClockBox: document.getElementById("whiteClockBox"),
    blackClock: document.getElementById("blackClock"),
    whiteClock: document.getElementById("whiteClock"),
    blackPlayerLabel: document.getElementById("blackPlayerLabel"),
    whitePlayerLabel: document.getElementById("whitePlayerLabel"),
    gameTurnMark: document.getElementById("gameTurnMark"),
    gameStatusText: document.getElementById("gameStatusText"),
    gameDetailText: document.getElementById("gameDetailText"),
    gameEngineLine: document.getElementById("gameEngineLine"),
    gameEngineInfo: document.getElementById("gameEngineInfo"),
    gameMoveCount: document.getElementById("gameMoveCount"),
    gameMoveList: document.getElementById("gameMoveList"),
    importButton: document.getElementById("importButton"),
    exportButton: document.getElementById("exportButton"),
    positionDialog: document.getElementById("positionDialog"),
    dialogTitle: document.getElementById("dialogTitle"),
    dialogSubtitle: document.getElementById("dialogSubtitle"),
    positionText: document.getElementById("positionText"),
    dialogError: document.getElementById("dialogError"),
    fileButton: document.getElementById("fileButton"),
    fileInput: document.getElementById("fileInput"),
    copyButton: document.getElementById("copyButton"),
    downloadButton: document.getElementById("downloadButton"),
    confirmImportButton: document.getElementById("confirmImportButton"),
    toastRegion: document.getElementById("toastRegion")
  };

  const ctx = elements.canvas.getContext("2d");
  const state = {
    board: createEmptyBoard(),
    sequence: [],
    moveSerial: 0,
    tool: "black",
    sideToMove: "black",
    rules: "freestyle",
    mode: "analysis",
    history: [],
    future: [],
    lastMove: null,
    hover: null,
    bestMove: null,
    highlightedMove: null,
    pvPreview: [],
    analyzing: false,
    analysisGeneration: 0,
    analysisRestartAt: 0,
    analysisDebounceWake: null,
    analysisLoopPromise: null,
    analysisCache: new Map(),
    modeRequestGeneration: 0,
    requestedMode: "analysis",
    modeTransitionPromise: null,
    searchJob: null,
    selectedCandidateKey: null,
    activePv: [],
    dialogMode: "import",
    renderMetrics: null,
    result: null,
    game: {
      status: "idle",
      players: { black: "human", white: "human" },
      clocks: { black: 300000, white: 300000 },
      increments: { black: 3000, white: 3000 },
      turnStartedAt: null,
      moves: [],
      generation: 0,
      engineThinking: false,
      inputPending: false,
      transitioning: false,
      timerId: null,
      engineTimerId: null,
      winner: null,
      reason: ""
    }
  };

  function createEmptyBoard() {
    return Array.from({ length: BOARD_SIZE }, () => Array(BOARD_SIZE).fill(EMPTY));
  }

  function copyBoard(board) {
    return board.map((row) => row.slice());
  }

  function boardRows(board = state.board) {
    return board.map((row) => row.map((cell) => cell === BLACK ? "B" : cell === WHITE ? "W" : ".").join(""));
  }

  function boardSnapshot() {
    return {
      board: copyBoard(state.board),
      sequence: state.sequence.map((move) => ({ ...move })),
      moveSerial: state.moveSerial,
      sideToMove: state.sideToMove,
      rules: state.rules,
      tool: state.tool,
      lastMove: state.lastMove ? { ...state.lastMove } : null
    };
  }

  function restoreSnapshot(snapshot) {
    if (state.game.status === "ended") resetGameState(false);
    state.board = copyBoard(snapshot.board);
    state.sequence = Array.isArray(snapshot.sequence) ? snapshot.sequence.map((move) => ({ ...move })) : [];
    state.moveSerial = Number.isInteger(snapshot.moveSerial) ? snapshot.moveSerial : state.sequence.length;
    state.sideToMove = snapshot.sideToMove;
    state.rules = snapshot.rules || state.rules;
    state.tool = snapshot.tool;
    state.lastMove = snapshot.lastMove ? { ...snapshot.lastMove } : null;
    state.bestMove = null;
    state.highlightedMove = null;
    state.pvPreview = [];
    state.selectedCandidateKey = null;
    state.activePv = [];
    syncControls();
    invalidateResult();
    updatePositionMeta();
    if (state.mode === "game") renderGameState();
    drawBoard();
  }

  function pushHistory() {
    state.history.push(boardSnapshot());
    if (state.history.length > 300) state.history.shift();
    state.future = [];
  }

  function undo() {
    if (!state.history.length || !canEditPosition()) return;
    state.future.push(boardSnapshot());
    restoreSnapshot(state.history.pop());
  }

  function redo() {
    if (!state.future.length || !canEditPosition()) return;
    state.history.push(boardSnapshot());
    restoreSnapshot(state.future.pop());
  }

  function clearBoard() {
    if (!canEditPosition() || !state.board.some((row) => row.some(Boolean))) return;
    if (state.game.status === "ended") resetGameState(false);
    pushHistory();
    state.board = createEmptyBoard();
    state.sequence = [];
    state.moveSerial = 0;
    state.lastMove = null;
    state.bestMove = null;
    state.highlightedMove = null;
    state.pvPreview = [];
    state.selectedCandidateKey = null;
    state.activePv = [];
    invalidateResult();
    updatePositionMeta();
    if (state.mode === "game") renderGameState();
    drawBoard();
    showToast("棋盘已清空");
  }

  function syncControls() {
    syncSegment(elements.stoneToolControl, "tool", state.tool);
    syncSegment(elements.ruleControl, "value", state.rules);
    syncSegment(elements.gameRuleControl, "value", state.rules);
    syncSegment(elements.sideControl, "value", state.sideToMove);
    syncSegment(elements.gameSideControl, "value", state.sideToMove);
    syncSegment(elements.blackPlayerControl, "value", state.game.players.black);
    syncSegment(elements.whitePlayerControl, "value", state.game.players.white);
  }

  function syncSegment(container, key, selectedValue) {
    container.querySelectorAll("button").forEach((button) => {
      const selected = button.dataset[key] === selectedValue;
      button.classList.toggle("is-selected", selected);
      button.setAttribute("aria-pressed", String(selected));
    });
  }

  function setSideToMove(side, syncTool = false) {
    const nextSide = side === "white" ? "white" : "black";
    if (nextSide === state.sideToMove) {
      if (syncTool) {
        state.tool = nextSide;
        syncControls();
        drawBoard();
      }
      return;
    }
    if (state.game.status === "ended") resetGameState(false);
    state.sideToMove = nextSide;
    if (syncTool) state.tool = state.sideToMove;
    syncControls();
    invalidateResult();
    updatePositionMeta();
    if (state.mode === "game") renderGameState();
    drawBoard();
  }

  function setRule(rule) {
    const nextRule = rule === "renju" ? "renju" : "freestyle";
    if (nextRule === state.rules) return;
    if (state.game.status === "ended") resetGameState(false);
    state.rules = nextRule;
    syncControls();
    invalidateResult();
    updatePositionMeta();
    if (state.mode === "game") renderGameState();
    drawBoard();
  }

  function applyEdit(x, y, value) {
    const previous = state.board[y][x];
    if (previous === value) return;
    if (state.game.status === "ended") resetGameState(false);
    pushHistory();
    state.board[y][x] = value;
    if (previous !== EMPTY) removeSequenceMove(x, y);
    if (value === EMPTY) {
      state.lastMove = latestSequenceMove();
    } else {
      const orderedMove = appendSequenceMove(x, y, value);
      state.lastMove = { ...orderedMove };
    }
    state.bestMove = null;
    state.highlightedMove = null;
    state.pvPreview = [];

    if (value !== EMPTY && elements.autoTurnInput.checked) {
      state.sideToMove = value === BLACK ? "white" : "black";
      state.tool = state.sideToMove;
      syncControls();
    }

    invalidateResult();
    updatePositionMeta();
    if (state.mode === "game") renderGameState();
    drawBoard();
  }

  function handleBoardEdit(point) {
    if (!point || !canEditPosition()) return;
    const { x, y } = point;
    if (state.tool === "erase") {
      applyEdit(x, y, EMPTY);
      return;
    }

    if (state.board[y][x] !== EMPTY) {
      showToast("该交叉点已有棋子", "error");
      return;
    }

    applyEdit(x, y, state.tool === "white" ? WHITE : BLACK);
  }

  function appendSequenceMove(x, y, color) {
    state.moveSerial += 1;
    const move = { x, y, color, number: state.moveSerial };
    state.sequence.push(move);
    return move;
  }

  function removeSequenceMove(x, y) {
    const index = state.sequence.findIndex((move) => move.x === x && move.y === y);
    if (index >= 0) state.sequence.splice(index, 1);
  }

  function latestSequenceMove() {
    if (!state.sequence.length) return null;
    const move = state.sequence.reduce((latest, item) => item.number > latest.number ? item : latest);
    return { ...move };
  }

  function canEditPosition() {
    return !state.game.transitioning && !["running", "paused"].includes(state.game.status);
  }

  function resizeCanvas() {
    const rect = elements.boardFrame.getBoundingClientRect();
    const side = Math.max(1, Math.floor(Math.min(rect.width, rect.height)));
    const dpr = Math.min(window.devicePixelRatio || 1, 2.5);
    const pixelSide = Math.round(side * dpr);
    if (elements.canvas.width !== pixelSide || elements.canvas.height !== pixelSide) {
      elements.canvas.width = pixelSide;
      elements.canvas.height = pixelSide;
    }
    elements.canvas.style.width = `${side}px`;
    elements.canvas.style.height = `${side}px`;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const margin = Math.max(23, side * 0.055);
    const spacing = (side - margin * 2) / (BOARD_SIZE - 1);
    state.renderMetrics = { side, dpr, margin, spacing };
    drawBoard();
  }

  function drawBoard() {
    const metrics = state.renderMetrics;
    if (!metrics) return;
    const { side, margin, spacing } = metrics;
    ctx.clearRect(0, 0, side, side);

    ctx.fillStyle = "#d9ad69";
    ctx.fillRect(0, 0, side, side);
    drawWoodTexture(side);

    ctx.save();
    ctx.strokeStyle = "rgba(77, 54, 30, 0.84)";
    ctx.lineWidth = Math.max(0.75, spacing * 0.026);
    ctx.beginPath();
    for (let i = 0; i < BOARD_SIZE; i += 1) {
      const coordinate = margin + i * spacing;
      ctx.moveTo(margin, coordinate);
      ctx.lineTo(side - margin, coordinate);
      ctx.moveTo(coordinate, margin);
      ctx.lineTo(coordinate, side - margin);
    }
    ctx.stroke();
    ctx.restore();

    drawStarPoints(margin, spacing);
    drawCoordinates(side, margin, spacing);

    const moveNumbers = new Map(state.sequence.map((move) => [`${move.x},${move.y}`, move.number]));
    for (let y = 0; y < BOARD_SIZE; y += 1) {
      for (let x = 0; x < BOARD_SIZE; x += 1) {
        if (state.board[y][x] !== EMPTY) {
          drawStone(x, y, state.board[y][x]);
          const number = moveNumbers.get(`${x},${y}`);
          if (elements.showMoveNumbersInput.checked && number != null) {
            drawStoneNumber(x, y, state.board[y][x], number);
          }
        }
      }
    }

    if (state.lastMove && state.board[state.lastMove.y][state.lastMove.x] !== EMPTY) {
      drawLastMove(state.lastMove);
    }
    if (state.pvPreview.length) drawPvPreview();
    if (state.bestMove) drawMoveMarker(state.bestMove, "best");
    if (state.highlightedMove) drawMoveMarker(state.highlightedMove, state.highlightedMove.forbidden ? "forbidden" : "focus");
    if (state.hover && canPreviewHover()) drawHover(state.hover);
  }

  function drawWoodTexture(side) {
    ctx.save();
    ctx.strokeStyle = "rgba(111, 71, 28, 0.075)";
    ctx.lineWidth = 0.7;
    for (let i = 0; i < 9; i += 1) {
      const y = ((i + 0.5) / 9) * side;
      ctx.beginPath();
      ctx.moveTo(0, y);
      for (let x = 0; x <= side; x += 18) {
        ctx.lineTo(x, y + Math.sin((x + i * 21) / 44) * 1.4);
      }
      ctx.stroke();
    }
    ctx.restore();
  }

  function drawCoordinates(side, margin, spacing) {
    const fontSize = Math.max(8, Math.min(11, spacing * 0.34));
    ctx.save();
    ctx.fillStyle = "rgba(72, 50, 26, 0.75)";
    ctx.font = `600 ${fontSize}px ui-monospace, SFMono-Regular, Menlo, monospace`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    for (let i = 0; i < BOARD_SIZE; i += 1) {
      const coordinate = margin + i * spacing;
      ctx.fillText(COLUMN_LABELS[i], coordinate, margin * 0.46);
      ctx.fillText(String(BOARD_SIZE - i), margin * 0.45, coordinate);
    }
    ctx.restore();
  }

  function drawStarPoints(margin, spacing) {
    const starPoints = [[3, 3], [11, 3], [7, 7], [3, 11], [11, 11]];
    ctx.save();
    ctx.fillStyle = "rgba(68, 46, 23, 0.9)";
    starPoints.forEach(([x, y]) => {
      ctx.beginPath();
      ctx.arc(margin + x * spacing, margin + y * spacing, Math.max(2.1, spacing * 0.08), 0, Math.PI * 2);
      ctx.fill();
    });
    ctx.restore();
  }

  function pointToPixel(move) {
    const { margin, spacing } = state.renderMetrics;
    return { px: margin + move.x * spacing, py: margin + move.y * spacing };
  }

  function drawStone(x, y, color, alpha = 1, scale = 1) {
    const { spacing } = state.renderMetrics;
    const { px, py } = pointToPixel({ x, y });
    const radius = spacing * 0.43 * scale;
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.shadowColor = "rgba(35, 26, 17, 0.35)";
    ctx.shadowBlur = Math.max(2, spacing * 0.11);
    ctx.shadowOffsetY = Math.max(1, spacing * 0.055);
    const gradient = ctx.createRadialGradient(
      px - radius * 0.34,
      py - radius * 0.38,
      radius * 0.12,
      px,
      py,
      radius
    );
    if (color === BLACK) {
      gradient.addColorStop(0, "#5b6163");
      gradient.addColorStop(0.35, "#2a2f31");
      gradient.addColorStop(1, "#101416");
    } else {
      gradient.addColorStop(0, "#ffffff");
      gradient.addColorStop(0.7, "#f3f3ef");
      gradient.addColorStop(1, "#c8c9c3");
    }
    ctx.fillStyle = gradient;
    ctx.beginPath();
    ctx.arc(px, py, radius, 0, Math.PI * 2);
    ctx.fill();
    ctx.shadowColor = "transparent";
    ctx.strokeStyle = color === BLACK ? "rgba(0, 0, 0, 0.52)" : "rgba(89, 87, 79, 0.52)";
    ctx.lineWidth = 0.8;
    ctx.stroke();
    ctx.restore();
  }

  function drawStoneNumber(x, y, color, number, alpha = 1) {
    const { spacing } = state.renderMetrics;
    const { px, py } = pointToPixel({ x, y });
    const text = String(number);
    const fontSize = text.length >= 3 ? spacing * 0.23 : text.length === 2 ? spacing * 0.27 : spacing * 0.31;
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.fillStyle = color === BLACK ? "#f5f7f6" : "#20272a";
    ctx.font = `700 ${Math.max(7, fontSize)}px ui-monospace, SFMono-Regular, Menlo, monospace`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(text, px, py + 0.5);
    ctx.restore();
  }

  function drawLastMove(move) {
    const { spacing } = state.renderMetrics;
    const { px, py } = pointToPixel(move);
    ctx.save();
    ctx.strokeStyle = move.color === WHITE ? "#b63239" : "#f2d06b";
    ctx.lineWidth = Math.max(1.4, spacing * 0.06);
    ctx.beginPath();
    ctx.arc(px, py, spacing * 0.13, 0, Math.PI * 2);
    ctx.stroke();
    ctx.restore();
  }

  function drawHover(move) {
    const existing = state.board[move.y][move.x];
    if (state.tool === "erase") {
      if (existing === EMPTY) return;
      const { spacing } = state.renderMetrics;
      const { px, py } = pointToPixel(move);
      ctx.save();
      ctx.strokeStyle = "rgba(183, 48, 58, 0.85)";
      ctx.lineWidth = Math.max(1.5, spacing * 0.06);
      const extent = spacing * 0.22;
      ctx.beginPath();
      ctx.moveTo(px - extent, py - extent);
      ctx.lineTo(px + extent, py + extent);
      ctx.moveTo(px + extent, py - extent);
      ctx.lineTo(px - extent, py + extent);
      ctx.stroke();
      ctx.restore();
      return;
    }
    if (existing !== EMPTY) return;
    drawStone(move.x, move.y, state.tool === "white" ? WHITE : BLACK, 0.42, 0.92);
  }

  function canPreviewHover() {
    if (state.game.status === "running") {
      return state.mode === "game"
        && state.game.players[state.sideToMove] === "human"
        && !state.game.inputPending;
    }
    return canEditPosition();
  }

  function drawMoveMarker(move, type) {
    if (!isMove(move)) return;
    const { spacing } = state.renderMetrics;
    const { px, py } = pointToPixel(move);
    const occupied = state.board[move.y][move.x] !== EMPTY;
    ctx.save();
    if (type === "forbidden") {
      ctx.strokeStyle = "#c52f3a";
      ctx.fillStyle = "rgba(197, 47, 58, 0.15)";
      ctx.lineWidth = Math.max(2, spacing * 0.075);
      ctx.beginPath();
      ctx.arc(px, py, spacing * 0.34, 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
      const extent = spacing * 0.16;
      ctx.beginPath();
      ctx.moveTo(px - extent, py - extent);
      ctx.lineTo(px + extent, py + extent);
      ctx.moveTo(px + extent, py - extent);
      ctx.lineTo(px - extent, py + extent);
      ctx.stroke();
    } else {
      ctx.strokeStyle = type === "best" ? "#08796b" : "#d65c23";
      ctx.fillStyle = occupied ? "transparent" : (type === "best" ? "rgba(8, 121, 107, 0.18)" : "rgba(214, 92, 35, 0.18)");
      ctx.lineWidth = Math.max(2, spacing * 0.07);
      ctx.beginPath();
      ctx.arc(px, py, spacing * (occupied ? 0.33 : 0.31), 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
    }
    ctx.restore();
  }

  function drawPvPreview() {
    const previewBoard = copyBoard(state.board);
    state.pvPreview.forEach((move, index) => {
      if (!isMove(move) || previewBoard[move.y][move.x] !== EMPTY) return;
      const color = move.color || ((index % 2 === 0) === (state.sideToMove === "black") ? BLACK : WHITE);
      previewBoard[move.y][move.x] = color;
      drawStone(move.x, move.y, color, 0.55, 0.86);
      if (elements.showMoveNumbersInput.checked) {
        drawStoneNumber(move.x, move.y, color, state.moveSerial + index + 1, 0.82);
      }
    });
  }

  function eventToPoint(event) {
    const metrics = state.renderMetrics;
    if (!metrics) return null;
    const rect = elements.canvas.getBoundingClientRect();
    const scaleX = metrics.side / rect.width;
    const scaleY = metrics.side / rect.height;
    const px = (event.clientX - rect.left) * scaleX;
    const py = (event.clientY - rect.top) * scaleY;
    const x = Math.round((px - metrics.margin) / metrics.spacing);
    const y = Math.round((py - metrics.margin) / metrics.spacing);
    if (x < 0 || x >= BOARD_SIZE || y < 0 || y >= BOARD_SIZE) return null;
    const dx = Math.abs(px - (metrics.margin + x * metrics.spacing));
    const dy = Math.abs(py - (metrics.margin + y * metrics.spacing));
    return dx <= metrics.spacing * 0.49 && dy <= metrics.spacing * 0.49 ? { x, y } : null;
  }

  function updatePositionMeta() {
    let black = 0;
    let white = 0;
    state.board.forEach((row) => row.forEach((cell) => {
      if (cell === BLACK) black += 1;
      if (cell === WHITE) white += 1;
    }));
    const total = black + white;
    elements.stoneCount.textContent = `黑 ${black} · 白 ${white} · 共 ${total} 手`;
    elements.lastMoveText.textContent = state.lastMove ? `末手 ${moveLabel(state.lastMove)}` : "末手 --";
    elements.undoButton.disabled = !canEditPosition() || state.history.length === 0;
    elements.redoButton.disabled = !canEditPosition() || state.future.length === 0;
    elements.clearButton.disabled = !canEditPosition() || total === 0;
    elements.positionHash.textContent = total === 0 ? "空棋盘" : `局面 ${positionDigest()}`;
  }

  function positionDigest() {
    let hash = 2166136261;
    const text = currentPositionKey();
    for (let i = 0; i < text.length; i += 1) {
      hash ^= text.charCodeAt(i);
      hash = Math.imul(hash, 16777619);
    }
    return (hash >>> 0).toString(16).padStart(8, "0").slice(0, 8).toUpperCase();
  }

  function currentPositionKey() {
    return positionKey(boardRows(), state.sideToMove, state.rules);
  }

  function payloadPositionKey(payload) {
    return positionKey(payload.board, payload.sideToMove, payload.rules);
  }

  function positionKey(rows, sideToMove, rules) {
    return `${rules}|${sideToMove}|${rows.join("/")}`;
  }

  function cloneAnalysisResult(result) {
    if (!result) return null;
    return {
      ...result,
      bestMove: result.bestMove ? { ...result.bestMove } : null,
      pv: result.pv.map((move) => ({ ...move })),
      candidates: result.candidates.map((candidate) => ({
        ...candidate,
        move: { ...candidate.move },
        pv: candidate.pv.map((move) => ({ ...move }))
      })),
      stats: { ...result.stats }
    };
  }

  function cacheAnalysisResult(key, result, options = {}) {
    if (!key || !result) return null;
    const existing = state.analysisCache.get(key);
    const incoming = {
      result: cloneAnalysisResult(result),
      quality: analysisCacheQuality(result, options)
    };
    const retained = shouldReplaceCachedAnalysis(existing, incoming) ? incoming : existing;
    state.analysisCache.delete(key);
    state.analysisCache.set(key, retained);
    while (state.analysisCache.size > ANALYSIS_CACHE_LIMIT) {
      const oldestKey = state.analysisCache.keys().next().value;
      state.analysisCache.delete(oldestKey);
    }
    return cloneAnalysisResult(retained.result);
  }

  function analysisCacheQuality(result, options) {
    return {
      depth: finiteNumber(result.stats?.depth) ?? -1,
      proven: result.proven === true,
      threatDepth: finiteNumber(options.threatDepth) ?? -1,
      candidateLimit: finiteNumber(options.candidateLimit) ?? -1,
      nodes: finiteNumber(result.stats?.nodes) ?? -1
    };
  }

  function shouldReplaceCachedAnalysis(existing, incoming) {
    if (!existing) return true;
    const existingSolved = existing.result.kind !== "heuristic";
    const incomingSolved = incoming.result.kind !== "heuristic";
    if (incomingSolved !== existingSolved) return incomingSolved;
    if (incoming.quality.proven !== existing.quality.proven) return incoming.quality.proven;
    if (incoming.quality.depth !== existing.quality.depth) {
      return incoming.quality.depth > existing.quality.depth;
    }
    const configurationNotWeaker = incoming.quality.threatDepth >= existing.quality.threatDepth
      && incoming.quality.candidateLimit >= existing.quality.candidateLimit;
    if (!configurationNotWeaker) return false;
    const configurationStronger = incoming.quality.threatDepth > existing.quality.threatDepth
      || incoming.quality.candidateLimit > existing.quality.candidateLimit;
    return configurationStronger || incoming.quality.nodes >= existing.quality.nodes;
  }

  function cachedAnalysisResult(key) {
    const cached = state.analysisCache.get(key);
    if (!cached) return null;
    state.analysisCache.delete(key);
    state.analysisCache.set(key, cached);
    return cloneAnalysisResult(cached.result);
  }

  function moveLabel(move) {
    return isMove(move) ? `${COLUMN_LABELS[move.x]}${BOARD_SIZE - move.y}` : "--";
  }

  function isMove(move) {
    return move && Number.isInteger(move.x) && Number.isInteger(move.y) && move.x >= 0 && move.x < BOARD_SIZE && move.y >= 0 && move.y < BOARD_SIZE;
  }

  function normalizeMove(input) {
    if (!input && input !== 0) return null;
    if (Array.isArray(input) && input.length >= 2) {
      const x = Number(input[0]);
      const y = Number(input[1]);
      return isMove({ x, y }) ? { x, y } : null;
    }
    if (typeof input === "string") {
      const match = input.trim().toUpperCase().match(/^([A-HJ-P])(1[0-5]|[1-9])$/);
      if (!match) return null;
      return { x: COLUMN_LABELS.indexOf(match[1]), y: BOARD_SIZE - Number(match[2]) };
    }
    if (typeof input === "object") {
      let x = Number(input.x ?? input.col ?? input.column);
      let y = Number(input.y ?? input.row);
      if (!Number.isInteger(x) || !Number.isInteger(y)) return null;
      const oneBased = input.oneBased === true || input.one_based === true;
      if (oneBased) { x -= 1; y -= 1; }
      const move = { x, y };
      const colorValue = input.color ?? input.player ?? input.side;
      if (colorValue !== undefined) move.color = normalizeCell(colorValue);
      return isMove(move) ? move : null;
    }
    return null;
  }

  function invalidateResult() {
    const shouldRestart = state.analyzing && state.mode === "analysis";
    if (shouldRestart) {
      state.analysisGeneration += 1;
      state.analysisRestartAt = Date.now() + ANALYSIS_RESTART_DEBOUNCE_MS;
    }
    state.bestMove = null;
    state.highlightedMove = null;
    state.pvPreview = [];
    state.selectedCandidateKey = null;
    state.activePv = [];
    const cached = cachedAnalysisResult(currentPositionKey());
    state.result = cached;
    if (cached) {
      renderResult(cached, false, shouldRestart ? "历史分析 · 正在继续" : "历史分析");
    } else if (shouldRestart) {
      showResultState("loading");
      elements.resultSubtitle.textContent = "局面已更新 · 正在切换搜索";
      elements.loadingDetail.textContent = "停止旧局面并准备最新局面...";
    } else {
      showResultState("empty");
    }
    if (shouldRestart) {
      setConnection("busy", "切换局面");
      cancelSearchJob("analysis");
    }
  }

  function showResultState(view) {
    elements.emptyState.hidden = view !== "empty";
    elements.loadingState.hidden = view !== "loading";
    elements.errorState.hidden = view !== "error";
    elements.resultContent.hidden = view !== "result";
    if (view !== "result") elements.resultKind.hidden = true;
    if (view === "empty") elements.resultSubtitle.textContent = "等待开始分析";
  }

  function analysisPayload(optionsOverride = {}) {
    return {
      size: BOARD_SIZE,
      board: boardRows(),
      rules: state.rules,
      sideToMove: state.sideToMove,
      options: {
        timeLimitMs: Math.round(clampNumber(elements.timeLimitInput.value, 0.1, 30, 2) * 1000),
        maxDepth: Math.round(clampNumber(elements.maxDepthInput.value, 1, 16, 5)),
        threatDepth: Math.round(clampNumber(elements.threatDepthInput.value, 1, 17, 9)),
        candidateLimit: 8,
        infinite: elements.infiniteAnalysisInput.checked,
        ...optionsOverride
      }
    };
  }

  function startAnalysis() {
    if (state.analyzing || state.analysisLoopPromise || state.searchJob || state.mode !== "analysis") return;
    state.analyzing = true;
    state.analysisGeneration += 1;
    state.analysisRestartAt = 0;
    state.bestMove = null;
    state.highlightedMove = null;
    state.pvPreview = [];
    state.selectedCandidateKey = null;
    state.activePv = [];
    drawBoard();
    setAnalyzeUi(true);
    const loop = runAnalysisLoop();
    state.analysisLoopPromise = loop;
    const clearLoop = () => {
      if (state.analysisLoopPromise === loop) state.analysisLoopPromise = null;
    };
    void loop.then(clearLoop, clearLoop);
  }

  async function stopAnalysis(showMessage = true) {
    if (!state.analyzing && state.searchJob?.owner !== "analysis" && !state.analysisLoopPromise) return;
    if (showMessage) showToast("正在停止分析");
    state.analyzing = false;
    state.analysisGeneration += 1;
    state.analysisRestartAt = 0;
    state.analysisDebounceWake?.();
    const loop = state.analysisLoopPromise;
    let stopError = null;
    try {
      await stopSearchJob("analysis");
    } catch (error) {
      stopError = error;
    }
    if (loop) await loop;
    if (stopError) throw stopError;
  }

  async function runAnalysisLoop() {
    let exitReason = "stopped";
    try {
      while (state.analyzing && state.mode === "analysis") {
        await waitForAnalysisRestartWindow();
        if (!state.analyzing || state.mode !== "analysis") break;
        const generation = state.analysisGeneration;
        const payload = analysisPayload();
        const key = payloadPositionKey(payload);
        normalizeNumberInputs(payload.options);
        presentAnalysisStart(key, payload);

        let outcome;
        try {
          outcome = await runSearchJob(payload, "analysis", (result) => {
            const retained = cacheAnalysisResult(key, result, payload.options);
            if (!isCurrentAnalysis(generation, key)) return;
            state.result = retained;
            renderResult(state.result, true);
            const depth = result.stats.depth == null ? "--" : Math.trunc(result.stats.depth);
            setConnection("busy", `深度 ${depth}`);
          });
        } catch (error) {
          if (preventsAnalysisRestart(error)) {
            showResultState("error");
            elements.resultSubtitle.textContent = "无法切换搜索局面";
            elements.errorMessage.textContent = error.message;
            setConnection("error", "停止失败");
            exitReason = "error";
            state.analyzing = false;
            break;
          }
          if (!state.analyzing || generation !== state.analysisGeneration) continue;
          showResultState("error");
          elements.resultSubtitle.textContent = "请求失败";
          elements.errorMessage.textContent = error.message || "无法连接分析服务";
          setConnection("error", "服务异常");
          exitReason = "error";
          state.analyzing = false;
          break;
        }

        if (!state.analyzing || state.mode !== "analysis") break;
        if (generation !== state.analysisGeneration || currentPositionKey() !== key) continue;
        if (outcome.cancelled) continue;
        if (!outcome.result) {
          showResultState("error");
          elements.resultSubtitle.textContent = "请求失败";
          elements.errorMessage.textContent = "搜索结束但未返回可用结果";
          setConnection("error", "服务异常");
          exitReason = "error";
          state.analyzing = false;
          break;
        }

        state.result = cacheAnalysisResult(key, outcome.result, payload.options);
        renderResult(state.result, false);
        setConnection("ok", state.result.stats.timedOut ? "已到时限" : "分析完成");
        exitReason = "completed";
        state.analyzing = false;
      }
    } catch (error) {
      exitReason = "error";
      state.analyzing = false;
      if (state.mode === "analysis") {
        showResultState("error");
        elements.resultSubtitle.textContent = "请求失败";
        elements.errorMessage.textContent = error?.message || "无法继续分析";
        setConnection("error", "服务异常");
      }
    } finally {
      if (!state.analyzing) {
        if (exitReason === "stopped") {
          if (state.result && state.mode === "analysis") renderResult(state.result, false, "分析已停止");
          else if (state.mode === "analysis") {
            showResultState("empty");
            elements.resultSubtitle.textContent = "分析已停止";
          }
          setConnection("idle", "已停止");
        }
        setAnalyzeUi(false);
      }
    }
  }

  function presentAnalysisStart(key, payload) {
    const cached = cachedAnalysisResult(key);
    if (cached) {
      state.result = cached;
      renderResult(cached, false, "历史分析 · 正在继续");
    } else {
      state.result = null;
      showResultState("loading");
      const sideName = payload.sideToMove === "black" ? "黑方" : "白方";
      elements.resultSubtitle.textContent = `${sideName}行棋 · 等待首层结果`;
      elements.loadingDetail.textContent = payload.options.infinite
        ? "无限分析，等待首层结果..."
        : `时间上限 ${(payload.options.timeLimitMs / 1000).toFixed(payload.options.timeLimitMs % 1000 ? 1 : 0)} 秒`;
    }
    setConnection("busy", "搜索中");
  }

  function isCurrentAnalysis(generation, key) {
    return state.analyzing
      && state.mode === "analysis"
      && generation === state.analysisGeneration
      && key === currentPositionKey();
  }

  function preventsAnalysisRestart(error) {
    return error?.name === "SearchStopError" || error?.preventAnalysisRestart === true;
  }

  async function waitForAnalysisRestartWindow() {
    while (state.analyzing) {
      const delay = state.analysisRestartAt - Date.now();
      if (delay <= 0) return;
      await new Promise((resolve) => {
        const finish = () => {
          window.clearTimeout(timerId);
          if (state.analysisDebounceWake === finish) state.analysisDebounceWake = null;
          resolve();
        };
        const timerId = window.setTimeout(finish, delay);
        state.analysisDebounceWake = finish;
      });
    }
  }

  async function runSearchJob(payload, owner, onUpdate) {
    if (state.searchJob) throw new Error("已有搜索任务正在运行");
    const job = {
      owner,
      jobId: null,
      cancelled: false,
      pollController: null,
      stopPromise: null,
      lastResult: null
    };
    state.searchJob = job;
    try {
      const started = await postJson("/api/analyze/start", payload, null, ANALYSIS_START_TIMEOUT_MS);
      job.jobId = started?.jobId ?? started?.job_id ?? null;
      if (job.jobId == null || job.jobId === "") throw new Error("后端未返回分析任务 ID");
      if (job.cancelled) {
        await ensureJobStopped(job);
        return { result: null, cancelled: true };
      }

      let version = 0;
      while (!job.cancelled) {
        const controller = new AbortController();
        job.pollController = controller;
        let update;
        try {
          update = await postJson(
            "/api/analyze/poll",
            { jobId: job.jobId, version },
            controller.signal,
            ANALYSIS_POLL_TIMEOUT_MS
          );
        } catch (error) {
          if (job.cancelled && error.name === "AbortError") break;
          throw error;
        } finally {
          if (job.pollController === controller) job.pollController = null;
        }
        if (Number.isFinite(Number(update?.version))) version = Number(update.version);
        if (update?.failed === true) throw new Error(update.error || "分析任务失败");
        if (update?.result && (update.changed !== false || !job.lastResult)) {
          job.lastResult = normalizeAnalysisResult(update.result, payload.sideToMove);
          if (typeof onUpdate === "function") onUpdate(job.lastResult, update);
        }
        if (update?.running === false) {
          return { result: job.lastResult, cancelled: false };
        }
        await waitForPoll(job, 180);
      }
      return { result: job.lastResult, cancelled: true };
    } catch (error) {
      if (job.cancelled && job.jobId == null && error?.name === "RequestTimeoutError") {
        error.preventAnalysisRestart = true;
      }
      if (!job.cancelled && job.jobId != null) {
        job.cancelled = true;
        job.pollController?.abort();
        await ensureJobStopped(job);
      }
      throw error;
    } finally {
      let stopError = null;
      if (job.cancelled && job.jobId != null) {
        try {
          await ensureJobStopped(job);
        } catch (error) {
          stopError = error;
        }
      }
      if (state.searchJob === job) state.searchJob = null;
      if (stopError) throw stopError;
    }
  }

  function cancelSearchJob(owner = null) {
    const job = state.searchJob;
    if (!job || (owner && job.owner !== owner)) return null;
    job.cancelled = true;
    job.pollController?.abort();
    return job;
  }

  async function stopSearchJob(owner = null) {
    const job = cancelSearchJob(owner);
    if (!job) return;
    if (job.jobId != null) await ensureJobStopped(job);
  }

  function ensureJobStopped(job) {
    if (!job.stopPromise) job.stopPromise = requestJobStop(job.jobId);
    return job.stopPromise;
  }

  async function requestJobStop(jobId) {
    let lastError = null;
    for (let attempt = 1; attempt <= ANALYSIS_STOP_ATTEMPTS; attempt += 1) {
      try {
        await postJson("/api/analyze/stop", { jobId }, null, ANALYSIS_STOP_TIMEOUT_MS);
        return;
      } catch (error) {
        if (error?.status === 404) return;
        lastError = error;
        if (attempt < ANALYSIS_STOP_ATTEMPTS) {
          await new Promise((resolve) => window.setTimeout(resolve, 120 * attempt));
        }
      }
    }
    const error = new Error(`停止搜索任务失败：${lastError?.message || "服务无响应"}`);
    error.name = "SearchStopError";
    error.cause = lastError;
    throw error;
  }

  function waitForPoll(job, delayMs) {
    return new Promise((resolve) => {
      window.setTimeout(resolve, job.cancelled ? 0 : delayMs);
    });
  }

  async function postJson(path, body, signal = null, timeoutMs = 0) {
    const controller = new AbortController();
    let timedOut = false;
    const relayAbort = () => controller.abort(signal?.reason);
    if (signal?.aborted) relayAbort();
    else signal?.addEventListener("abort", relayAbort, { once: true });
    const timeoutId = timeoutMs > 0 ? window.setTimeout(() => {
      timedOut = true;
      controller.abort();
    }, timeoutMs) : null;

    try {
      const response = await fetch(path, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
        signal: controller.signal
      });
      if (!response.ok) throw await httpError(response);
      if (response.status === 204) return {};
      const text = await response.text();
      if (!text.trim()) return {};
      let result;
      try {
        result = JSON.parse(text);
      } catch (_) {
        throw new Error("后端返回了无效的 JSON 结果");
      }
      if (!result || typeof result !== "object") throw new Error("后端返回了无效的 JSON 结果");
      return result;
    } catch (error) {
      if (signal?.aborted) {
        if (error?.name === "AbortError") throw error;
        const abortError = new Error("请求已取消");
        abortError.name = "AbortError";
        throw abortError;
      }
      if (timedOut) {
        const timeoutError = new Error(`请求超时（${timeoutMs} ms）`);
        timeoutError.name = "RequestTimeoutError";
        throw timeoutError;
      }
      throw error;
    } finally {
      if (timeoutId != null) window.clearTimeout(timeoutId);
      signal?.removeEventListener("abort", relayAbort);
    }
  }

  function setAnalyzeUi(running) {
    elements.analyzeButton.hidden = running;
    elements.stopButton.hidden = !running;
    elements.timeLimitInput.disabled = running || elements.infiniteAnalysisInput.checked;
    elements.maxDepthInput.disabled = running;
    elements.threatDepthInput.disabled = running;
    elements.infiniteAnalysisInput.disabled = running;
    updateInteractionUi();
    syncInfiniteUi();
  }

  function normalizeNumberInputs(options) {
    elements.timeLimitInput.value = String(options.timeLimitMs / 1000);
    elements.maxDepthInput.value = String(options.maxDepth);
    elements.threatDepthInput.value = String(options.threatDepth);
  }

  function syncInfiniteUi() {
    elements.timeLimitInput.disabled = state.analyzing || elements.infiniteAnalysisInput.checked;
  }

  function clampNumber(value, min, max, fallback) {
    const number = Number(value);
    return Number.isFinite(number) ? Math.min(max, Math.max(min, number)) : fallback;
  }

  async function httpError(response) {
    let message = `HTTP ${response.status}`;
    try {
      const contentType = response.headers.get("content-type") || "";
      if (contentType.includes("application/json")) {
        const body = await response.json();
        message = body.message || body.error || body.detail || message;
      } else {
        const text = (await response.text()).trim();
        if (text) message = text.slice(0, 240);
      }
    } catch (_) {
      // Keep the HTTP status when the error body cannot be parsed.
    }
    const error = new Error(message);
    error.status = response.status;
    return error;
  }

  function normalizeAnalysisResult(raw, fallbackSide = state.sideToMove) {
    const stats = raw.stats || raw.statistics || {};
    const mateSource = raw.mate || raw.provenMate || raw.proven_mate || null;
    const resultType = String(raw.kind ?? raw.status ?? raw.type ?? raw.resultType ?? raw.result_type ?? "")
      .trim()
      .toLowerCase()
      .replace(/-/g, "_");
    const drawFlag = ["draw", "proven_draw", "terminal_draw"].includes(resultType);
    const forcedKind = ["forced_mate", "forced_win"].includes(resultType);
    const explicitWinner = normalizeSide(mateSource?.winner ?? raw.winner);
    const hasMateField = raw.mate != null
      || raw.provenMate != null
      || raw.proven_mate != null
      || raw.mateIn != null
      || raw.mate_in != null;
    const mateFlag = !drawFlag && (forcedKind || Boolean(explicitWinner && hasMateField));
    const terminalFlag = drawFlag
      || mateFlag
      || raw.proven === true
      || raw.terminal === true
      || ["terminal", "terminal_win", "win"].includes(resultType);
    const winner = explicitWinner || (forcedKind ? fallbackSide : null);
    const mateInRaw = mateSource?.moves ?? mateSource?.plies ?? mateSource?.mateIn ?? mateSource?.mate_in ?? raw.mateIn ?? raw.mate_in;
    const mateIn = mateInRaw != null && mateInRaw !== "" && Number.isFinite(Number(mateInRaw))
      ? Math.abs(Math.trunc(Number(mateInRaw)))
      : null;
    const bestMove = normalizeMove(raw.bestMove ?? raw.best_move ?? raw.move);
    const pvRaw = raw.pv ?? raw.principalVariation ?? raw.principal_variation ?? [];
    const pv = Array.isArray(pvRaw) ? pvRaw.map(normalizeMove).filter(Boolean) : [];
    const candidatesRaw = raw.candidates ?? raw.moves ?? raw.topMoves ?? raw.top_moves ?? [];
    const candidates = Array.isArray(candidatesRaw) ? candidatesRaw.map(normalizeCandidate).filter(Boolean).slice(0, 12) : [];
    const rates = normalizeWinRates(raw, fallbackSide);
    const evaluation = finiteNumber(raw.evaluation ?? raw.eval ?? raw.score ?? raw.value);

    const terminalWinner = raw.terminal === true && Boolean(winner) && mateIn === 0;
    return {
      kind: drawFlag ? "draw" : terminalWinner ? "terminal" : mateFlag ? "mate" : terminalFlag ? "terminal" : "heuristic",
      proven: drawFlag || mateFlag || raw.proven === true,
      terminal: raw.terminal === true,
      winner,
      mateIn,
      bestMove: bestMove || pv[0] || candidates[0]?.move || null,
      pv,
      candidates,
      blackWinRate: rates.black,
      whiteWinRate: rates.white,
      evaluation,
      message: raw.message || raw.summary || "",
      stats: {
        nodes: finiteNumber(stats.nodes ?? raw.nodes),
        depth: finiteNumber(stats.depth ?? stats.completedDepth ?? stats.completed_depth ?? raw.depth),
        elapsedMs: finiteNumber(stats.elapsedMs ?? stats.elapsed_ms ?? stats.timeMs ?? stats.time_ms ?? raw.elapsedMs),
        cutoffs: finiteNumber(stats.cutoffs ?? stats.betaCutoffs ?? stats.beta_cutoffs),
        ttHits: finiteNumber(stats.ttHits ?? stats.tt_hits ?? stats.transpositionHits ?? stats.transposition_hits),
        nps: finiteNumber(stats.nps ?? stats.nodesPerSecond ?? stats.nodes_per_second),
        timedOut: stats.timedOut === true || stats.timed_out === true
      }
    };
  }

  function normalizeCandidate(candidate, index) {
    const move = normalizeMove(candidate?.move ?? candidate?.position ?? candidate?.coord ?? candidate);
    if (!move) return null;
    const mateValue = candidate?.mateIn ?? candidate?.mate_in ?? candidate?.mate;
    const mateIn = Number.isFinite(Number(mateValue)) ? Math.abs(Math.trunc(Number(mateValue))) : null;
    const rawRate = candidate?.winRate ?? candidate?.win_rate ?? candidate?.probability;
    const winRate = rawRate == null ? null : normalizeRate(rawRate);
    const winRateScore = finiteNumber(candidate?.winRateScore ?? candidate?.win_rate_score);
    const pvRaw = candidate?.pv ?? candidate?.principalVariation ?? candidate?.principal_variation ?? [];
    const pv = Array.isArray(pvRaw) ? pvRaw.map(normalizeMove).filter(Boolean) : [];
    if (!pv.length || pv[0].x !== move.x || pv[0].y !== move.y) pv.unshift({ ...move });
    return {
      move,
      score: finiteNumber(candidate?.score ?? candidate?.evaluation ?? candidate?.eval),
      winRateScore,
      winRate,
      mateIn,
      rank: finiteNumber(candidate?.rank) || index + 1,
      pv
    };
  }

  function normalizeWinRates(raw, fallbackSide = state.sideToMove) {
    const rates = raw.winRates || raw.win_rates || raw.probabilities || {};
    let black = rates.black ?? rates.blackWin ?? rates.black_win ?? raw.blackWinRate ?? raw.black_win_rate;
    let white = rates.white ?? rates.whiteWin ?? rates.white_win ?? raw.whiteWinRate ?? raw.white_win_rate;
    black = black == null ? null : normalizeRate(black);
    white = white == null ? null : normalizeRate(white);

    if (black == null && white == null) {
      const score = finiteNumber(raw.winRateScore ?? raw.win_rate_score ?? raw.evaluation ?? raw.eval ?? raw.score ?? raw.value);
      if (score != null) {
        const perspective = normalizeSide(raw.scorePerspective ?? raw.score_perspective ?? fallbackSide);
        const sideRate = scoreToWinRate(score);
        black = perspective === "white" ? 1 - sideRate : sideRate;
        white = 1 - black;
      } else {
        black = 0.5;
        white = 0.5;
      }
    } else if (black == null) {
      black = 1 - white;
    } else if (white == null) {
      white = 1 - black;
    }

    const total = black + white;
    if (total > 0) {
      black /= total;
      white /= total;
    }
    return { black: clampNumber(black, 0, 1, 0.5), white: clampNumber(white, 0, 1, 0.5) };
  }

  function normalizeRate(value) {
    const number = Number(value);
    if (!Number.isFinite(number)) return 0.5;
    return clampNumber(number > 1 ? number / 100 : number, 0, 1, 0.5);
  }

  function scoreToWinRate(value) {
    const score = clampNumber(value, -SCORE_LOGISTIC_BOUND, SCORE_LOGISTIC_BOUND, 0);
    return 1 / (1 + Math.exp(-score / SCORE_LOGISTIC_SCALE));
  }

  function finiteNumber(value) {
    if (value == null || value === "") return null;
    const number = Number(value);
    return Number.isFinite(number) ? number : null;
  }

  function normalizeSide(value) {
    const normalized = String(value ?? "").toLowerCase();
    if (["white", "w", "2", "白", "白方"].includes(normalized)) return "white";
    if (["black", "b", "1", "黑", "黑方"].includes(normalized)) return "black";
    return null;
  }

  function renderResult(result, running = false, completionLabel = null) {
    showResultState("result");
    const elapsed = formatDuration(result.stats.elapsedMs);
    if (running) {
      const depth = result.stats.depth == null ? "--" : Math.trunc(result.stats.depth);
      elements.resultSubtitle.textContent = `搜索中 · 已完成深度 ${depth}${elapsed === "--" ? "" : ` · ${elapsed}`}`;
    } else if (completionLabel) {
      const depth = result.stats.depth == null ? "" : ` · 深度 ${Math.trunc(result.stats.depth)}`;
      elements.resultSubtitle.textContent = `${completionLabel}${depth}${elapsed === "--" ? "" : ` · ${elapsed}`}`;
    } else if (result.stats.timedOut) {
      const depth = result.stats.depth == null ? "" : ` · 完成深度 ${Math.trunc(result.stats.depth)}`;
      elements.resultSubtitle.textContent = `时间用尽${depth}${elapsed === "--" ? "" : ` · ${elapsed}`}`;
    } else {
      elements.resultSubtitle.textContent = elapsed === "--" ? "搜索完成" : `搜索完成 · ${elapsed}`;
    }
    elements.resultKind.hidden = false;
    elements.resultKind.textContent = running ? "计算中" : result.proven ? "已证明" : "启发式";
    elements.resultKind.style.color = !running && result.proven ? "#a14b0d" : "";
    elements.resultKind.style.background = !running && result.proven ? "#fff0dd" : "";

    if (result.kind === "mate") {
      const winnerName = result.winner === "white" ? "白方" : result.winner === "black" ? "黑方" : "当前方";
      elements.verdictLabel.textContent = "强制胜负";
      elements.verdictValue.textContent = result.mateIn == null ? `${winnerName}必胜` : `${winnerName} ${result.mateIn} 步杀`;
      const lengthNote = result.mateIn == null ? "搜索已证明强制变化" : "步数按双方合计总落子数计算";
      elements.verdictNote.textContent = result.message ? `${lengthNote} · ${result.message}` : lengthNote;
      elements.winrateBlock.hidden = true;
      elements.verdictBlock.dataset.verdict = "mate";
    } else if (result.kind === "draw") {
      elements.verdictLabel.textContent = "终局结果";
      elements.verdictValue.textContent = "和棋";
      elements.verdictNote.textContent = result.message || "搜索已证明双方均无法取胜";
      elements.winrateBlock.hidden = true;
      elements.verdictBlock.dataset.verdict = "draw";
    } else if (result.kind === "terminal") {
      const winnerName = result.winner === "white" ? "白方" : result.winner === "black" ? "黑方" : null;
      elements.verdictLabel.textContent = "终局结果";
      elements.verdictValue.textContent = winnerName ? `${winnerName}胜` : "终局已证明";
      elements.verdictNote.textContent = result.terminal && result.winner
        ? "当前局面已有胜线"
        : result.message || "当前局面已经结束";
      elements.winrateBlock.hidden = true;
      elements.verdictBlock.dataset.verdict = "terminal";
    } else {
      const blackRate = result.blackWinRate;
      const favored = Math.abs(blackRate - 0.5) < 0.055 ? "局面接近均势" : blackRate > 0.5 ? "黑方占优" : "白方占优";
      elements.verdictLabel.textContent = "局面评估";
      elements.verdictValue.textContent = favored;
      elements.verdictNote.textContent = result.message || heuristicNote(blackRate);
      elements.winrateBlock.hidden = false;
      elements.verdictBlock.dataset.verdict = "heuristic";
    }

    elements.bestMoveButton.textContent = moveLabel(result.bestMove);
    elements.bestMoveButton.disabled = !result.bestMove;
    state.bestMove = result.bestMove;
    renderWinRates(result.blackWinRate, result.whiteWinRate);
    const selectedCandidate = result.candidates.find((candidate) => candidateKey(candidate.move) === state.selectedCandidateKey);
    if (selectedCandidate) renderPv(selectedCandidate.pv, selectedCandidate.move, true);
    else {
      state.selectedCandidateKey = null;
      state.highlightedMove = null;
      state.pvPreview = [];
      renderPv(result.pv);
    }
    renderCandidates(result.candidates, result);
    renderStats(result.stats);
    drawBoard();
  }

  function heuristicNote(blackRate) {
    const delta = Math.abs(blackRate - 0.5);
    if (delta < 0.055) return "双方机会相近";
    if (delta < 0.18) return "优势有限，变化仍然复杂";
    if (delta < 0.35) return "优势明显，但尚未证明杀棋";
    return "胜势显著，但结果仍为启发式估计";
  }

  function renderWinRates(black, white) {
    const blackPercent = clampNumber(black, 0, 1, 0.5) * 100;
    const whitePercent = clampNumber(white, 0, 1, 0.5) * 100;
    elements.blackWinRate.textContent = `${blackPercent.toFixed(1)}%`;
    elements.whiteWinRate.textContent = `${whitePercent.toFixed(1)}%`;
    elements.blackWinBar.style.width = `${blackPercent}%`;
    elements.whiteWinBar.style.width = `${whitePercent}%`;
    elements.blackWinBar.parentElement.setAttribute("aria-label", `黑方 ${blackPercent.toFixed(1)}%，白方 ${whitePercent.toFixed(1)}%`);
  }

  function renderPv(pv, branchMove = null, previewBranch = false) {
    state.activePv = pv.map((move) => ({ ...move }));
    elements.pvLine.replaceChildren();
    elements.pvTitle.textContent = branchMove ? `候选 ${moveLabel(branchMove)} 分支` : "主变化";
    elements.backToMainPvButton.hidden = !branchMove;
    elements.pvDepth.textContent = pv.length ? `${pv.length} 手` : "无变化";
    if (previewBranch) {
      state.pvPreview = pv.map((move) => ({ ...move }));
      state.highlightedMove = branchMove;
    }
    if (!pv.length) {
      const empty = document.createElement("span");
      empty.className = "candidate-score";
      empty.textContent = "后端未返回主变化";
      elements.pvLine.appendChild(empty);
      return;
    }
    pv.forEach((move, index) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "pv-move";
      button.dataset.index = String(index + 1);
      const swatch = document.createElement("i");
      const color = (index % 2 === 0) === (state.sideToMove === "black") ? "black" : "white";
      swatch.className = `stone-swatch ${color}`;
      button.append(swatch, document.createTextNode(moveLabel(move)));
      button.title = `预览至第 ${index + 1} 手`;
      button.addEventListener("click", () => {
        const alreadyActive = button.classList.contains("is-active");
        elements.pvLine.querySelectorAll("button").forEach((item) => item.classList.remove("is-active"));
        state.pvPreview = alreadyActive ? [] : pv.slice(0, index + 1);
        if (!alreadyActive) button.classList.add("is-active");
        drawBoard();
      });
      elements.pvLine.appendChild(button);
    });
  }

  function renderCandidates(candidates, result) {
    elements.candidateList.replaceChildren();
    elements.candidateCount.textContent = `${candidates.length} 项`;
    if (!candidates.length) {
      const empty = document.createElement("span");
      empty.className = "candidate-score";
      empty.textContent = "后端未返回候选着法";
      elements.candidateList.appendChild(empty);
      return;
    }

    const perspectiveBlack = state.sideToMove === "black";
    candidates.forEach((candidate, index) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "candidate-row";
      const key = candidateKey(candidate.move);
      button.classList.toggle("is-active", key === state.selectedCandidateKey);
      button.classList.toggle("has-pv", candidate.pv.length > 1);
      button.title = candidate.pv.length > 1
        ? `打开 ${moveLabel(candidate.move)} 的具体分支`
        : `在棋盘上定位 ${moveLabel(candidate.move)}`;

      const rank = document.createElement("span");
      rank.className = "candidate-rank";
      rank.textContent = String(candidate.rank || index + 1);

      const coordinate = document.createElement("strong");
      coordinate.className = "candidate-coordinate";
      coordinate.textContent = moveLabel(candidate.move);

      let meterRate = candidate.winRate;
      if (meterRate == null) {
        const rateScore = candidate.winRateScore ?? candidate.score;
        if (rateScore != null) meterRate = scoreToWinRate(rateScore);
        else meterRate = perspectiveBlack ? result.blackWinRate : result.whiteWinRate;
      }
      const meter = document.createElement("span");
      meter.className = "candidate-meter";
      const fill = document.createElement("i");
      fill.style.width = `${clampNumber(meterRate, 0, 1, 0.5) * 100}%`;
      meter.appendChild(fill);

      const score = document.createElement("span");
      score.className = "candidate-score";
      if (candidate.mateIn != null) score.textContent = `${candidate.mateIn} 步杀`;
      else if (candidate.winRate != null) score.textContent = `${(candidate.winRate * 100).toFixed(1)}%`;
      else if (candidate.score != null) score.textContent = formatSigned(candidate.score);
      else score.textContent = "--";

      button.append(rank, coordinate, meter, score);
      button.addEventListener("click", () => {
        elements.candidateList.querySelectorAll("button").forEach((item) => item.classList.remove("is-active"));
        button.classList.add("is-active");
        state.selectedCandidateKey = key;
        state.highlightedMove = candidate.move;
        renderPv(candidate.pv, candidate.move, true);
        drawBoard();
        scrollBoardIntoView();
      });
      elements.candidateList.appendChild(button);
    });
  }

  function candidateKey(move) {
    return isMove(move) ? `${move.x},${move.y}` : "";
  }

  function showMainVariation() {
    if (!state.result) return;
    state.selectedCandidateKey = null;
    state.highlightedMove = null;
    state.pvPreview = [];
    renderPv(state.result.pv);
    elements.candidateList.querySelectorAll("button").forEach((item) => item.classList.remove("is-active"));
    drawBoard();
  }

  function renderStats(stats) {
    elements.statNodes.textContent = formatCompact(stats.nodes);
    elements.statDepth.textContent = stats.depth == null ? "--" : String(Math.trunc(stats.depth));
    elements.statTime.textContent = formatDuration(stats.elapsedMs);
    elements.statCutoffs.textContent = formatCompact(stats.cutoffs);
    elements.statTtHits.textContent = formatCompact(stats.ttHits);
    elements.statNps.textContent = formatCompact(stats.nps);
  }

  function formatSigned(value) {
    const rounded = Math.abs(value) >= 100 ? Math.round(value) : Number(value).toFixed(1);
    return value > 0 ? `+${rounded}` : String(rounded);
  }

  function formatCompact(value) {
    if (value == null || !Number.isFinite(Number(value))) return "--";
    return new Intl.NumberFormat("zh-CN", { notation: "compact", maximumFractionDigits: 1 }).format(Number(value));
  }

  function formatDuration(ms) {
    if (ms == null || !Number.isFinite(Number(ms))) return "--";
    const value = Number(ms);
    if (value < 1000) return `${Math.round(value)} ms`;
    return `${(value / 1000).toFixed(value < 10000 ? 2 : 1)} s`;
  }

  function setConnection(status, text) {
    elements.connectionStatus.dataset.state = status;
    elements.connectionStatus.querySelector("span").textContent = text;
  }

  function showToast(message, type = "info") {
    const toast = document.createElement("div");
    toast.className = `toast${type === "error" ? " error" : ""}`;
    toast.textContent = message;
    elements.toastRegion.appendChild(toast);
    window.setTimeout(() => toast.remove(), 2800);
  }

  function runUiAction(promise, fallbackMessage) {
    void Promise.resolve(promise).catch((error) => {
      showToast(error?.message || fallbackMessage, "error");
      setConnection("error", fallbackMessage);
    });
  }

  function scrollBoardIntoView() {
    if (window.matchMedia("(max-width: 760px)").matches) {
      elements.boardFrame.scrollIntoView({ behavior: "smooth", block: "center" });
    }
  }

  function positionDocument() {
    return {
      format: "gomoku-position-v1",
      size: BOARD_SIZE,
      rules: state.rules,
      sideToMove: state.sideToMove,
      board: boardRows(),
      lastMove: state.lastMove ? { x: state.lastMove.x, y: state.lastMove.y } : null,
      options: {
        timeLimitMs: Math.round(clampNumber(elements.timeLimitInput.value, 0.1, 30, 2) * 1000),
        maxDepth: Math.round(clampNumber(elements.maxDepthInput.value, 1, 16, 5)),
        threatDepth: Math.round(clampNumber(elements.threatDepthInput.value, 1, 17, 9))
      }
    };
  }

  function openImportDialog() {
    if (!canEditPosition()) {
      showToast("请先暂停并结束当前操作", "error");
      return;
    }
    state.dialogMode = "import";
    elements.dialogTitle.textContent = "导入局面";
    elements.dialogSubtitle.textContent = "JSON 或 15 行棋盘文本";
    elements.positionText.value = "";
    elements.positionText.readOnly = false;
    elements.dialogError.hidden = true;
    elements.fileButton.hidden = false;
    elements.copyButton.hidden = true;
    elements.downloadButton.hidden = true;
    elements.confirmImportButton.hidden = false;
    elements.fileInput.value = "";
    elements.positionDialog.showModal();
    window.setTimeout(() => elements.positionText.focus(), 0);
  }

  function openExportDialog() {
    state.dialogMode = "export";
    elements.dialogTitle.textContent = "导出局面";
    elements.dialogSubtitle.textContent = "gomoku-position-v1 JSON";
    elements.positionText.value = JSON.stringify(positionDocument(), null, 2);
    elements.positionText.readOnly = true;
    elements.dialogError.hidden = true;
    elements.fileButton.hidden = true;
    elements.copyButton.hidden = false;
    elements.downloadButton.hidden = false;
    elements.confirmImportButton.hidden = true;
    elements.positionDialog.showModal();
  }

  function parsePosition(text) {
    const trimmed = text.trim();
    if (!trimmed) throw new Error("局面数据不能为空");
    let source;
    if (trimmed.startsWith("{") || trimmed.startsWith("[")) {
      try {
        source = JSON.parse(trimmed);
      } catch (error) {
        throw new Error(`JSON 格式错误：${error.message}`);
      }
    } else {
      source = { board: trimmed.split(/\r?\n/).filter((line) => line.trim()) };
    }

    if (Array.isArray(source) && source.length === BOARD_SIZE) source = { board: source };
    if (!source || typeof source !== "object") throw new Error("无法识别局面格式");

    let board;
    if (source.board) board = parseBoard(source.board);
    else if (Array.isArray(source.moves)) board = boardFromMoves(source.moves);
    else throw new Error("局面中缺少 board 或 moves 字段");

    const sideValue = source.sideToMove ?? source.side_to_move ?? source.turn;
    const sideToMove = sideValue == null ? state.sideToMove : normalizeSide(sideValue);
    if (!sideToMove) throw new Error("sideToMove 只能是 black 或 white");

    const ruleValue = source.rules ?? source.rule;
    let rules = state.rules;
    if (ruleValue != null) {
      const normalizedRule = String(ruleValue).trim().toLowerCase();
      if (["renju", "forbidden", "restricted", "有禁手"].includes(normalizedRule)) rules = "renju";
      else if (["freestyle", "free", "gomoku", "无禁手"].includes(normalizedRule)) rules = "freestyle";
      else throw new Error("rules 只能是 freestyle 或 renju");
    }
    const lastMove = normalizeMove(source.lastMove ?? source.last_move);
    const options = source.options == null ? {} : source.options;
    if (!options || typeof options !== "object" || Array.isArray(options)) throw new Error("options 必须是对象");
    return { board, sideToMove, rules, lastMove, options };
  }

  function parseBoard(input) {
    if (!Array.isArray(input) || input.length !== BOARD_SIZE) throw new Error("棋盘必须恰好包含 15 行");
    return input.map((row, y) => {
      let cells;
      if (typeof row === "string") {
        const compact = row.replace(/[\s,|]/g, "");
        cells = Array.from(compact);
      } else if (Array.isArray(row)) {
        cells = row;
      } else {
        throw new Error(`第 ${y + 1} 行格式无效`);
      }
      if (cells.length !== BOARD_SIZE) throw new Error(`第 ${y + 1} 行必须包含 15 个交叉点`);
      return cells.map((cell, x) => {
        const value = normalizeCell(cell);
        if (value == null) throw new Error(`第 ${y + 1} 行第 ${x + 1} 列棋子符号无效`);
        return value;
      });
    });
  }

  function boardFromMoves(moves) {
    const board = createEmptyBoard();
    moves.forEach((item, index) => {
      const move = normalizeMove(item);
      if (!move) throw new Error(`第 ${index + 1} 手坐标无效`);
      if (board[move.y][move.x] !== EMPTY) throw new Error(`第 ${index + 1} 手重复落子`);
      board[move.y][move.x] = move.color || (index % 2 === 0 ? BLACK : WHITE);
    });
    return board;
  }

  function normalizeCell(cell) {
    if (cell === EMPTY || cell === "." || cell === "_" || cell === "-" || cell === null) return EMPTY;
    const value = String(cell).trim().toLowerCase();
    if (["b", "black", "x", "1", "黑"].includes(value)) return BLACK;
    if (["w", "white", "o", "2", "白"].includes(value)) return WHITE;
    if (["", ".", "_", "-", "0"].includes(value)) return EMPTY;
    return null;
  }

  function importPosition() {
    try {
      const parsed = parsePosition(elements.positionText.value);
      pushHistory();
      state.board = parsed.board;
      state.sequence = [];
      state.moveSerial = 0;
      state.sideToMove = parsed.sideToMove;
      state.rules = parsed.rules;
      state.tool = parsed.sideToMove;
      state.lastMove = parsed.lastMove && state.board[parsed.lastMove.y][parsed.lastMove.x] !== EMPTY
        ? { ...parsed.lastMove, color: state.board[parsed.lastMove.y][parsed.lastMove.x] }
        : null;
      resetGameState(true);

      const options = parsed.options;
      const importedTime = options.timeLimitMs ?? options.time_limit_ms;
      const importedDepth = options.maxDepth ?? options.max_depth;
      const importedThreatDepth = options.threatDepth ?? options.threat_depth;
      if (importedTime != null) elements.timeLimitInput.value = String(clampNumber(Number(importedTime) / 1000, 0.1, 30, 2));
      if (importedDepth != null) elements.maxDepthInput.value = String(Math.round(clampNumber(importedDepth, 1, 16, 5)));
      if (importedThreatDepth != null) elements.threatDepthInput.value = String(Math.round(clampNumber(importedThreatDepth, 1, 17, 9)));

      syncControls();
      invalidateResult();
      updatePositionMeta();
      drawBoard();
      elements.positionDialog.close();
      showToast("局面已载入");
    } catch (error) {
      elements.dialogError.textContent = error.message;
      elements.dialogError.hidden = false;
    }
  }

  async function copyExport() {
    try {
      await navigator.clipboard.writeText(elements.positionText.value);
      showToast("局面 JSON 已复制");
    } catch (_) {
      elements.positionText.select();
      const copied = document.execCommand("copy");
      showToast(copied ? "局面 JSON 已复制" : "复制失败，请手动复制", copied ? "info" : "error");
    }
  }

  function downloadExport() {
    const blob = new Blob([elements.positionText.value], { type: "application/json;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `gomoku-${positionDigest().toLowerCase()}.json`;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    URL.revokeObjectURL(url);
  }

  function setMode(mode) {
    state.requestedMode = mode === "game" ? "game" : "analysis";
    state.modeRequestGeneration += 1;
    if (state.modeTransitionPromise) return state.modeTransitionPromise;
    const transition = runModeTransitionLoop();
    state.modeTransitionPromise = transition;
    const clearTransition = () => {
      if (state.modeTransitionPromise === transition) state.modeTransitionPromise = null;
    };
    void transition.then(clearTransition, clearTransition);
    return transition;
  }

  async function runModeTransitionLoop() {
    while (state.requestedMode !== state.mode) {
      const requestGeneration = state.modeRequestGeneration;
      const nextMode = state.requestedMode;
      await new Promise((resolve) => window.setTimeout(resolve, MODE_SWITCH_DEBOUNCE_MS));
      if (!isCurrentModeRequest(requestGeneration, nextMode)) continue;

      let rollbackGame = null;
      const resumeAnalysisOnStale = nextMode === "game" && state.analyzing;
      try {
        if (nextMode === "game" && (state.analyzing || state.analysisLoopPromise || state.searchJob?.owner === "analysis")) {
          await stopAnalysis(false);
        } else if (nextMode === "analysis" && ["running", "paused"].includes(state.game.status)) {
          rollbackGame = await finishGameSessionForAnalysis();
        }
      } catch (error) {
        if (isCurrentModeRequest(requestGeneration, nextMode)) {
          state.requestedMode = state.mode;
          state.modeRequestGeneration += 1;
          syncSegment(elements.modeTabs, "mode", state.mode);
          showToast(error?.message || "无法切换工作模式", "error");
          setConnection("error", "切换失败");
        } else if (preventsAnalysisRestart(error)) {
          state.requestedMode = state.mode;
          state.modeRequestGeneration += 1;
          syncSegment(elements.modeTabs, "mode", state.mode);
          showToast(error.message, "error");
          setConnection("error", "停止失败");
        }
        return;
      }

      if (!isCurrentModeRequest(requestGeneration, nextMode)) {
        rollbackGame?.();
        if (resumeAnalysisOnStale
          && state.requestedMode === "analysis"
          && state.mode === "analysis"
          && !state.analyzing
          && !state.searchJob) {
          startAnalysis();
        }
        continue;
      }
      commitMode(nextMode);
    }
  }

  function commitMode(nextMode) {
    state.mode = nextMode;
    syncSegment(elements.modeTabs, "mode", state.mode);
    elements.analysisSettings.hidden = state.mode !== "analysis";
    elements.gameSettings.hidden = state.mode !== "game";
    elements.analysisResults.hidden = state.mode !== "analysis";
    elements.gameResultContent.hidden = state.mode !== "game";
    elements.settingsHeading.textContent = state.mode === "analysis" ? "分析设置" : "对弈设置";
    elements.resultsHeading.textContent = state.mode === "analysis" ? "分析结果" : "对局状态";
    elements.resultKind.hidden = state.mode !== "analysis" || elements.resultContent.hidden;
    if (state.mode === "game") renderGameState();
    else invalidateResult();
    updateInteractionUi();
    drawBoard();
  }

  function isCurrentModeRequest(generation, mode) {
    return generation === state.modeRequestGeneration && mode === state.requestedMode;
  }

  async function finishGameSessionForAnalysis() {
    if (!["running", "paused"].includes(state.game.status)) return null;
    const previousStatus = state.game.status;
    if (state.game.status === "running" && state.game.turnStartedAt != null) {
      const side = state.sideToMove;
      const elapsed = Math.max(0, Date.now() - state.game.turnStartedAt);
      state.game.clocks[side] = Math.max(0, state.game.clocks[side] - elapsed);
    }
    const snapshot = {
      status: previousStatus,
      clocks: { ...state.game.clocks },
      winner: state.game.winner,
      reason: state.game.reason
    };
    state.game.generation += 1;
    state.game.status = "ended";
    state.game.winner = null;
    state.game.reason = "当前棋盘已转为分析分支";
    state.game.turnStartedAt = null;
    state.game.engineThinking = false;
    state.game.inputPending = false;
    state.game.transitioning = true;
    clearEngineSchedule();
    stopGameClockTimer();
    cancelSearchJob("game");
    renderGameState();
    updateInteractionUi();

    try {
      await stopSearchJob("game");
    } catch (error) {
      restoreGameSessionAfterModeCancellation(snapshot, false);
      throw error;
    }
    state.game.transitioning = false;
    renderGameState();
    updateInteractionUi();
    return () => restoreGameSessionAfterModeCancellation(snapshot);
  }

  function restoreGameSessionAfterModeCancellation(snapshot, resumeRunningGame = true) {
    const restoredStatus = snapshot.status === "running" && !resumeRunningGame ? "paused" : snapshot.status;
    state.game.generation += 1;
    state.game.status = restoredStatus;
    state.game.clocks = { ...snapshot.clocks };
    state.game.winner = snapshot.winner;
    state.game.reason = snapshot.reason;
    state.game.engineThinking = false;
    state.game.inputPending = false;
    state.game.transitioning = false;
    state.game.turnStartedAt = restoredStatus === "running" ? Date.now() : null;
    if (restoredStatus === "running") startGameClockTimer();
    else stopGameClockTimer();
    renderGameState();
    updateInteractionUi();
    drawBoard();
    setConnection(restoredStatus === "running" ? "ok" : "idle", restoredStatus === "running" ? "对局进行中" : "对局暂停");
    if (restoredStatus === "running") scheduleEngineTurn();
  }

  function setPlayerType(side, type) {
    if (["running", "paused"].includes(state.game.status)) return;
    state.game.players[side] = type === "engine" ? "engine" : "human";
    syncControls();
    renderGameState();
  }

  async function startGame() {
    if (["running", "paused"].includes(state.game.status)) return;
    if (state.analyzing) await stopAnalysis(false);
    if (state.searchJob) return;

    state.game.generation += 1;
    state.game.status = "running";
    state.game.winner = null;
    state.game.reason = "";
    state.game.moves = [];
    state.game.engineThinking = false;
    state.game.inputPending = false;
    state.game.transitioning = false;
    state.game.clocks.black = Math.round(clampNumber(elements.blackInitialInput.value, 0.1, 180, 5) * 60000);
    state.game.clocks.white = Math.round(clampNumber(elements.whiteInitialInput.value, 0.1, 180, 5) * 60000);
    state.game.increments.black = Math.round(clampNumber(elements.blackIncrementInput.value, 0, 60, 3) * 1000);
    state.game.increments.white = Math.round(clampNumber(elements.whiteIncrementInput.value, 0, 60, 3) * 1000);
    normalizeGameInputs();
    state.tool = state.sideToMove;
    state.game.turnStartedAt = Date.now();
    state.bestMove = null;
    state.highlightedMove = null;
    state.pvPreview = [];
    syncControls();

    const existing = existingBoardOutcome();
    if (existing) {
      finishGame(existing.winner, existing.reason);
      return;
    }
    if (boardIsFull()) {
      finishGame(null, "棋盘已满，和棋");
      return;
    }

    startGameClockTimer();
    renderGameState();
    updateInteractionUi();
    setConnection("ok", "对局进行中");
    scheduleEngineTurn();
  }

  async function pauseGame(silent = false) {
    if (state.game.status !== "running" || state.game.transitioning) return;
    if (!freezeCurrentClock()) return;
    state.game.status = "paused";
    state.game.transitioning = true;
    state.game.generation += 1;
    clearEngineSchedule();
    state.game.engineThinking = false;
    stopGameClockTimer();
    renderGameState();
    updateInteractionUi();
    setConnection("idle", "对局暂停");
    try {
      await stopSearchJob("game");
    } finally {
      state.game.transitioning = false;
      if (state.game.status === "paused") {
        renderGameState();
        updateInteractionUi();
        if (!silent) showToast("对局已暂停");
      }
    }
  }

  function resumeGame() {
    if (state.game.status !== "paused" || state.game.transitioning) return;
    if (state.analyzing || state.searchJob) {
      showToast("请先停止当前分析", "error");
      return;
    }
    state.game.status = "running";
    state.game.generation += 1;
    state.game.turnStartedAt = Date.now();
    startGameClockTimer();
    renderGameState();
    updateInteractionUi();
    setConnection("ok", "对局进行中");
    scheduleEngineTurn();
  }

  function endGame() {
    if (!["running", "paused"].includes(state.game.status)) return;
    if (state.game.status === "running") freezeCurrentClock();
    finishGame(null, "对局已结束");
  }

  function resetGameState(update = true) {
    state.game.generation += 1;
    clearEngineSchedule();
    stopGameClockTimer();
    cancelSearchJob("game");
    state.game.status = "idle";
    state.game.winner = null;
    state.game.reason = "";
    state.game.moves = [];
    state.game.engineThinking = false;
    state.game.inputPending = false;
    state.game.transitioning = false;
    state.game.turnStartedAt = null;
    state.game.clocks.black = Math.round(clampNumber(elements.blackInitialInput.value, 0.1, 180, 5) * 60000);
    state.game.clocks.white = Math.round(clampNumber(elements.whiteInitialInput.value, 0.1, 180, 5) * 60000);
    if (update) {
      renderGameState();
      updateInteractionUi();
    }
  }

  function finishGame(winner, reason) {
    if (state.game.status === "ended") return;
    state.game.generation += 1;
    state.game.status = "ended";
    state.game.winner = winner;
    state.game.reason = reason || "对局结束";
    state.game.turnStartedAt = null;
    state.game.engineThinking = false;
    state.game.inputPending = false;
    state.game.transitioning = false;
    state.bestMove = null;
    clearEngineSchedule();
    stopGameClockTimer();
    cancelSearchJob("game");
    renderGameState();
    updateInteractionUi();
    drawBoard();
    setConnection(winner ? "ok" : "idle", winner ? `${sideName(winner)}胜` : "对局结束");
  }

  async function attemptHumanGameMove(move) {
    if (!isMove(move) || state.game.status !== "running" || state.game.inputPending) return;
    const side = state.sideToMove;
    if (state.game.players[side] !== "human") return;
    if (state.board[move.y][move.x] !== EMPTY) {
      showToast("该交叉点已有棋子", "error");
      return;
    }
    state.game.inputPending = true;
    const generation = state.game.generation;
    try {
      const legality = await checkMoveLegality(move, side);
      if (generation !== state.game.generation || state.game.status !== "running" || state.sideToMove !== side) return;
      if (!legality.legal) {
        showToast(legality.reason || "该着法不合法", "error");
        return;
      }
      commitGameMove(move, legality.winning);
    } catch (error) {
      showToast(error.message || "无法检查该着法", "error");
      setConnection("error", "规则检查失败");
    } finally {
      state.game.inputPending = false;
    }
  }

  function commitGameMove(move, winningHint = false) {
    if (state.game.status !== "running" || state.board[move.y][move.x] !== EMPTY) return;
    const side = state.sideToMove;
    const color = side === "black" ? BLACK : WHITE;
    const spent = consumeCurrentClock();
    if (spent == null) return;

    state.board[move.y][move.x] = color;
    const orderedMove = appendSequenceMove(move.x, move.y, color);
    state.lastMove = { ...orderedMove };
    state.game.moves.push({ ...orderedMove, side, spentMs: spent });
    state.game.clocks[side] += state.game.increments[side];
    state.bestMove = null;
    state.highlightedMove = null;
    state.pvPreview = [];
    state.result = null;

    const won = winningHint || isWinningPlacementLocal(move, color);
    updatePositionMeta();
    renderGameMoves();
    if (won) {
      finishGame(side, `${sideName(side)}形成五连`);
      return;
    }
    if (boardIsFull()) {
      finishGame(null, "棋盘已满，和棋");
      return;
    }

    state.sideToMove = oppositeSide(side);
    state.tool = state.sideToMove;
    state.game.turnStartedAt = Date.now();
    syncControls();
    renderGameState();
    drawBoard();
    scheduleEngineTurn();
  }

  async function checkMoveLegality(move, side) {
    if (!isMove(move) || state.board[move.y][move.x] !== EMPTY) {
      return { legal: false, winning: false, reason: "该交叉点已有棋子" };
    }
    const color = side === "black" ? BLACK : WHITE;
    if (state.rules !== "renju" || side !== "black") {
      return { legal: true, winning: isWinningPlacementLocal(move, color), reason: "" };
    }
    const result = await postJson("/api/forbidden", {
      size: BOARD_SIZE,
      board: boardRows(),
      rules: state.rules,
      sideToMove: side,
      move: { x: move.x, y: move.y }
    });
    return {
      legal: result.legal === true && result.forbidden !== true,
      winning: result.winning === true,
      reason: result.reason || (result.forbidden ? "黑方禁手" : "该着法不合法")
    };
  }

  function scheduleEngineTurn() {
    clearEngineSchedule();
    if (state.game.status !== "running" || state.game.players[state.sideToMove] !== "engine") return;
    const generation = state.game.generation;
    state.game.engineTimerId = window.setTimeout(() => {
      state.game.engineTimerId = null;
      if (generation === state.game.generation) void runEngineTurn(generation);
    }, 90);
  }

  function clearEngineSchedule() {
    if (state.game.engineTimerId != null) {
      window.clearTimeout(state.game.engineTimerId);
      state.game.engineTimerId = null;
    }
  }

  async function runEngineTurn(generation) {
    if (generation !== state.game.generation || state.game.status !== "running") return;
    const side = state.sideToMove;
    if (state.game.players[side] !== "engine") return;
    if (state.searchJob) {
      scheduleEngineTurn();
      return;
    }
    const remaining = liveClock(side);
    if (remaining <= 0) {
      flagOnTime(side);
      return;
    }
    const budget = engineMoveBudget(side, remaining);
    const payload = analysisPayload({ timeLimitMs: budget, infinite: false, candidateLimit: 8 });
    const key = payloadPositionKey(payload);
    state.game.engineThinking = true;
    state.bestMove = null;
    renderGameState();
    setConnection("busy", `${sideName(side)}引擎思考`);

    try {
      const outcome = await runSearchJob(payload, "game", (result) => {
        const retained = cacheAnalysisResult(key, result, payload.options);
        if (generation !== state.game.generation || state.sideToMove !== side) return;
        state.bestMove = retained.bestMove;
        const depth = result.stats.depth == null ? "--" : Math.trunc(result.stats.depth);
        const nodes = formatCompact(result.stats.nodes);
        elements.gameEngineInfo.textContent = `深度 ${depth} · ${nodes} 节点 · ${moveLabel(retained.bestMove)}`;
        drawBoard();
      });
      if (outcome.cancelled || generation !== state.game.generation || state.game.status !== "running" || state.sideToMove !== side) return;
      if (!outcome.result) {
        finishGame(null, `${sideName(side)}引擎未返回结果`);
        return;
      }
      const result = cacheAnalysisResult(key, outcome.result, payload.options);
      state.bestMove = result.bestMove;
      const choices = uniqueMoves([result.bestMove, ...result.candidates.map((candidate) => candidate.move)]);
      let selected = null;
      let winning = false;
      for (const move of choices) {
        if (generation !== state.game.generation || state.game.status !== "running") return;
        const legality = await checkMoveLegality(move, side);
        if (legality.legal) {
          selected = move;
          winning = legality.winning;
          break;
        }
      }
      if (!selected) {
        finishGame(null, `${sideName(side)}引擎没有合法着法`);
        return;
      }
      commitGameMove(selected, winning);
    } catch (error) {
      if (generation === state.game.generation && state.game.status === "running") {
        finishGame(null, `${sideName(side)}引擎错误：${error.message || "搜索失败"}`);
        setConnection("error", "引擎错误");
      }
    } finally {
      if (generation === state.game.generation) {
        state.game.engineThinking = false;
        renderGameState();
      }
    }
  }

  function uniqueMoves(moves) {
    const seen = new Set();
    return moves.filter((move) => {
      if (!isMove(move)) return false;
      const key = candidateKey(move);
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    });
  }

  function engineMoveBudget(side, remaining) {
    const increment = state.game.increments[side];
    const reserve = Math.min(1200, remaining * 0.08);
    const available = Math.max(100, remaining - reserve);
    const allocation = remaining / 18 + increment * 0.8;
    return Math.max(100, Math.floor(Math.min(30000, available, allocation)));
  }

  function consumeCurrentClock() {
    const side = state.sideToMove;
    const startedAt = state.game.turnStartedAt;
    const spent = startedAt == null ? 0 : Math.max(0, Date.now() - startedAt);
    state.game.clocks[side] = Math.max(0, state.game.clocks[side] - spent);
    state.game.turnStartedAt = null;
    if (state.game.clocks[side] <= 0) {
      flagOnTime(side);
      return null;
    }
    return spent;
  }

  function freezeCurrentClock() {
    return consumeCurrentClock() != null;
  }

  function liveClock(side) {
    let remaining = state.game.clocks[side];
    if (state.game.status === "running" && side === state.sideToMove && state.game.turnStartedAt != null) {
      remaining -= Date.now() - state.game.turnStartedAt;
    }
    return Math.max(0, remaining);
  }

  function flagOnTime(side) {
    state.game.clocks[side] = 0;
    finishGame(oppositeSide(side), `${sideName(side)}超时`);
  }

  function startGameClockTimer() {
    stopGameClockTimer();
    state.game.timerId = window.setInterval(updateGameClocks, 100);
    updateGameClocks();
  }

  function stopGameClockTimer() {
    if (state.game.timerId != null) {
      window.clearInterval(state.game.timerId);
      state.game.timerId = null;
    }
  }

  function updateGameClocks() {
    const black = liveClock("black");
    const white = liveClock("white");
    elements.blackClock.textContent = formatClock(black);
    elements.whiteClock.textContent = formatClock(white);
    updateClockBox(elements.blackClockBox, "black", black);
    updateClockBox(elements.whiteClockBox, "white", white);
    if (state.game.status === "running" && liveClock(state.sideToMove) <= 0) flagOnTime(state.sideToMove);
  }

  function updateClockBox(element, side, remaining) {
    element.classList.toggle("is-active", state.game.status === "running" && state.sideToMove === side);
    element.classList.toggle("is-low", remaining > 0 && remaining < 30000);
    element.classList.toggle("is-flagged", state.game.status === "ended" && remaining <= 0);
  }

  function formatClock(ms) {
    const safe = Math.max(0, Math.ceil(ms / 100) * 100);
    const minutes = Math.floor(safe / 60000);
    const seconds = Math.floor((safe % 60000) / 1000);
    if (safe < 60000) {
      const tenths = Math.floor((safe % 1000) / 100);
      return `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}.${tenths}`;
    }
    return `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`;
  }

  function renderGameState() {
    updateGameClocks();
    elements.blackPlayerLabel.textContent = state.game.players.black === "engine" ? "引擎" : "玩家";
    elements.whitePlayerLabel.textContent = state.game.players.white === "engine" ? "引擎" : "玩家";
    const markColor = state.sideToMove === "white" ? "white" : "black";
    elements.gameTurnMark.replaceChildren();
    const mark = document.createElement("i");
    mark.className = `stone-swatch ${markColor}`;
    elements.gameTurnMark.appendChild(mark);

    if (state.game.status === "running") {
      const player = state.game.players[state.sideToMove] === "engine" ? "引擎" : "玩家";
      elements.gameStatusText.textContent = `${sideName(state.sideToMove)}行棋`;
      elements.gameDetailText.textContent = state.game.engineThinking ? `${player}正在计算` : `等待${player}落子`;
    } else if (state.game.status === "paused") {
      elements.gameStatusText.textContent = "对局已暂停";
      elements.gameDetailText.textContent = `${sideName(state.sideToMove)}继续行棋`;
    } else if (state.game.status === "ended") {
      elements.gameStatusText.textContent = state.game.winner ? `${sideName(state.game.winner)}胜` : "对局结束";
      elements.gameDetailText.textContent = state.game.reason;
    } else {
      elements.gameStatusText.textContent = "尚未开始";
      elements.gameDetailText.textContent = "从当前局面开始对弈";
    }
    elements.gameEngineLine.hidden = !state.game.engineThinking;
    elements.gameEngineInfo.textContent ||= "准备搜索";
    renderGameMoves();
    updateGameControlButtons();
  }

  function renderGameMoves() {
    elements.gameMoveList.replaceChildren();
    elements.gameMoveCount.textContent = `${state.game.moves.length} 手`;
    if (!state.game.moves.length) {
      const empty = document.createElement("p");
      empty.textContent = "尚无对局着法";
      elements.gameMoveList.appendChild(empty);
      return;
    }
    const rows = [];
    state.game.moves.forEach((move) => {
      if (move.side === "black" || !rows.length || rows[rows.length - 1].white) rows.push({ black: null, white: null });
      rows[rows.length - 1][move.side] = move;
    });
    rows.forEach((row, index) => {
      const line = document.createElement("div");
      line.className = "game-move-row";
      const number = document.createElement("span");
      number.textContent = String(index + 1);
      line.append(number, gameMoveButton(row.black, "black"), gameMoveButton(row.white, "white"));
      elements.gameMoveList.appendChild(line);
    });
    elements.gameMoveList.scrollTop = elements.gameMoveList.scrollHeight;
  }

  function gameMoveButton(move, side) {
    const button = document.createElement("button");
    button.type = "button";
    if (!move) {
      button.disabled = true;
      button.textContent = "--";
      return button;
    }
    const swatch = document.createElement("i");
    swatch.className = `stone-swatch ${side}`;
    button.append(swatch, document.createTextNode(moveLabel(move)));
    button.addEventListener("click", () => {
      state.highlightedMove = move;
      drawBoard();
      scrollBoardIntoView();
    });
    return button;
  }

  function updateGameControlButtons() {
    const active = ["running", "paused"].includes(state.game.status);
    elements.startGameButton.hidden = active;
    elements.pauseGameButton.hidden = !active;
    elements.endGameButton.hidden = !active;
    elements.pauseGameButton.textContent = state.game.status === "paused" ? "继续" : "暂停";
    elements.startGameButton.disabled = state.analyzing || Boolean(state.searchJob);
    elements.pauseGameButton.disabled = state.game.transitioning
      || (state.game.status === "paused" && (state.analyzing || Boolean(state.searchJob)));
    [
      elements.blackInitialInput,
      elements.blackIncrementInput,
      elements.whiteInitialInput,
      elements.whiteIncrementInput
    ].forEach((input) => { input.disabled = active; });
    [elements.blackPlayerControl, elements.whitePlayerControl].forEach((control) => {
      control.querySelectorAll("button").forEach((button) => { button.disabled = active; });
    });
  }

  function updateInteractionUi() {
    const locked = !canEditPosition();
    elements.stoneToolControl.querySelectorAll("button").forEach((button) => { button.disabled = locked; });
    [elements.ruleControl, elements.gameRuleControl, elements.sideControl, elements.gameSideControl].forEach((control) => {
      control.querySelectorAll("button").forEach((button) => { button.disabled = locked; });
    });
    elements.autoTurnInput.disabled = locked;
    updatePositionMeta();
    updateGameControlButtons();
  }

  function normalizeGameInputs() {
    elements.blackInitialInput.value = String(state.game.clocks.black / 60000);
    elements.whiteInitialInput.value = String(state.game.clocks.white / 60000);
    elements.blackIncrementInput.value = String(state.game.increments.black / 1000);
    elements.whiteIncrementInput.value = String(state.game.increments.white / 1000);
  }

  function previewIdleClocks() {
    if (state.game.status !== "idle" && state.game.status !== "ended") return;
    state.game.clocks.black = Math.round(clampNumber(elements.blackInitialInput.value, 0.1, 180, 5) * 60000);
    state.game.clocks.white = Math.round(clampNumber(elements.whiteInitialInput.value, 0.1, 180, 5) * 60000);
    updateGameClocks();
  }

  function existingBoardOutcome() {
    let blackWins = false;
    let whiteWins = false;
    for (let y = 0; y < BOARD_SIZE; y += 1) {
      for (let x = 0; x < BOARD_SIZE; x += 1) {
        const color = state.board[y][x];
        if (color === EMPTY) continue;
        if (hasWinningLineAt({ x, y }, color)) {
          if (color === BLACK) blackWins = true;
          else whiteWins = true;
        }
      }
    }
    if (blackWins && whiteWins) return { winner: null, reason: "当前局面双方均已有胜线" };
    if (blackWins) return { winner: "black", reason: "当前局面黑方已有胜线" };
    if (whiteWins) return { winner: "white", reason: "当前局面白方已有胜线" };
    return null;
  }

  function isWinningPlacementLocal(move, color) {
    const previous = state.board[move.y][move.x];
    state.board[move.y][move.x] = color;
    const won = hasWinningLineAt(move, color);
    state.board[move.y][move.x] = previous;
    return won;
  }

  function hasWinningLineAt(move, color) {
    const directions = [[1, 0], [0, 1], [1, 1], [1, -1]];
    return directions.some(([dx, dy]) => {
      const length = 1 + countDirection(move, color, dx, dy) + countDirection(move, color, -dx, -dy);
      return state.rules === "renju" && color === BLACK ? length === 5 : length >= 5;
    });
  }

  function countDirection(move, color, dx, dy) {
    let count = 0;
    let x = move.x + dx;
    let y = move.y + dy;
    while (x >= 0 && x < BOARD_SIZE && y >= 0 && y < BOARD_SIZE && state.board[y][x] === color) {
      count += 1;
      x += dx;
      y += dy;
    }
    return count;
  }

  function boardIsFull() {
    return state.board.every((row) => row.every((cell) => cell !== EMPTY));
  }

  function oppositeSide(side) {
    return side === "white" ? "black" : "white";
  }

  function sideName(side) {
    return side === "white" ? "白方" : "黑方";
  }

  function bindEvents() {
    elements.canvas.addEventListener("pointermove", (event) => {
      state.hover = eventToPoint(event);
      elements.cursorCoordinate.textContent = state.hover ? `坐标 ${moveLabel(state.hover)}` : "坐标 --";
      drawBoard();
    });
    elements.canvas.addEventListener("pointerleave", () => {
      state.hover = null;
      elements.cursorCoordinate.textContent = "坐标 --";
      drawBoard();
    });
    elements.canvas.addEventListener("pointerdown", (event) => {
      if (event.pointerType === "touch") event.preventDefault();
    });
    elements.canvas.addEventListener("click", (event) => {
      const point = eventToPoint(event);
      if (state.mode === "game" && state.game.status === "running") void attemptHumanGameMove(point);
      else handleBoardEdit(point);
    });

    elements.stoneToolControl.addEventListener("click", (event) => {
      const button = event.target.closest("button[data-tool]");
      if (!button || !canEditPosition()) return;
      state.tool = button.dataset.tool;
      syncControls();
      drawBoard();
    });
    [elements.ruleControl, elements.gameRuleControl].forEach((control) => {
      control.addEventListener("click", (event) => {
        const button = event.target.closest("button[data-value]");
        if (button && canEditPosition()) setRule(button.dataset.value);
      });
    });
    [elements.sideControl, elements.gameSideControl].forEach((control) => {
      control.addEventListener("click", (event) => {
        const button = event.target.closest("button[data-value]");
        if (button && canEditPosition()) setSideToMove(button.dataset.value, elements.autoTurnInput.checked);
      });
    });
    elements.modeTabs.addEventListener("click", (event) => {
      const button = event.target.closest("button[data-mode]");
      if (button) runUiAction(setMode(button.dataset.mode), "模式切换失败");
    });
    elements.blackPlayerControl.addEventListener("click", (event) => {
      const button = event.target.closest("button[data-value]");
      if (button) setPlayerType("black", button.dataset.value);
    });
    elements.whitePlayerControl.addEventListener("click", (event) => {
      const button = event.target.closest("button[data-value]");
      if (button) setPlayerType("white", button.dataset.value);
    });
    elements.autoTurnInput.addEventListener("change", () => {
      if (elements.autoTurnInput.checked && state.tool !== "erase") {
        state.tool = state.sideToMove;
        syncControls();
        drawBoard();
      }
    });
    elements.showMoveNumbersInput.addEventListener("change", drawBoard);
    elements.infiniteAnalysisInput.addEventListener("change", syncInfiniteUi);

    elements.undoButton.addEventListener("click", undo);
    elements.redoButton.addEventListener("click", redo);
    elements.clearButton.addEventListener("click", clearBoard);
    elements.analyzeButton.addEventListener("click", startAnalysis);
    elements.stopButton.addEventListener("click", () => runUiAction(stopAnalysis(true), "停止失败"));
    elements.backToMainPvButton.addEventListener("click", showMainVariation);
    elements.bestMoveButton.addEventListener("click", () => {
      if (!state.result?.bestMove) return;
      state.highlightedMove = state.result.bestMove;
      drawBoard();
      scrollBoardIntoView();
    });
    elements.startGameButton.addEventListener("click", () => runUiAction(startGame(), "启动对局失败"));
    elements.pauseGameButton.addEventListener("click", () => {
      if (state.game.status === "paused") resumeGame();
      else runUiAction(pauseGame(), "暂停失败");
    });
    elements.endGameButton.addEventListener("click", endGame);
    [elements.blackInitialInput, elements.whiteInitialInput].forEach((input) => {
      input.addEventListener("input", previewIdleClocks);
      input.addEventListener("change", previewIdleClocks);
    });

    elements.importButton.addEventListener("click", openImportDialog);
    elements.exportButton.addEventListener("click", openExportDialog);
    elements.confirmImportButton.addEventListener("click", importPosition);
    elements.copyButton.addEventListener("click", copyExport);
    elements.downloadButton.addEventListener("click", downloadExport);
    elements.fileInput.addEventListener("change", async () => {
      const file = elements.fileInput.files[0];
      if (!file) return;
      try {
        elements.positionText.value = await file.text();
        elements.dialogError.hidden = true;
      } catch (_) {
        elements.dialogError.textContent = "无法读取所选文件";
        elements.dialogError.hidden = false;
      }
    });
    elements.positionDialog.addEventListener("click", (event) => {
      if (event.target === elements.positionDialog) elements.positionDialog.close();
    });

    document.addEventListener("keydown", (event) => {
      const typing = /INPUT|TEXTAREA|SELECT/.test(document.activeElement?.tagName);
      if (typing || elements.positionDialog.open) return;
      const modifier = event.ctrlKey || event.metaKey;
      if (modifier && event.key.toLowerCase() === "z") {
        event.preventDefault();
        if (event.shiftKey) redo();
        else undo();
      } else if (modifier && event.key.toLowerCase() === "y") {
        event.preventDefault();
        redo();
      } else if (event.key === "Escape" && state.analyzing) {
        runUiAction(stopAnalysis(true), "停止失败");
      } else if (event.key === "Escape" && state.game.status === "running") {
        runUiAction(pauseGame(), "暂停失败");
      }
    });

    const observer = new ResizeObserver(resizeCanvas);
    observer.observe(elements.boardFrame);
    window.addEventListener("resize", resizeCanvas, { passive: true });
  }

  syncControls();
  updatePositionMeta();
  bindEvents();
  syncInfiniteUi();
  renderGameState();
  updateInteractionUi();
  resizeCanvas();
})();
