// socketio.js — Emscripten JS library for Socket.io client bridge
// Include via emcc --js-library socketio.js

mergeInto(LibraryManager.library, {

  // Initialize Socket.io connection to the server
  SocketIO_Init: function(urlPtr) {
    var url = UTF8ToString(urlPtr);
    window._socket = io(url);

    window._socket.on('connect', function() {
      if (window._onConnect) window._onConnect();
    });

    window._socket.on('assigned', function(data) {
      if (window._onAssigned) window._onAssigned(data.socketId, data.cryptidIndex);
    });

    window._socket.on('player_joined', function(data) {
      if (window._onPlayerJoined) {
        window._onPlayerJoined(data.socketId, data.cryptidIndex, data.x, data.y);
      }
    });

    window._socket.on('player_left', function(data) {
      if (window._onPlayerLeft) window._onPlayerLeft(data.socketId);
    });

    window._socket.on('state', function(state) {
      if (window._onState) {
        // Serialize state object to JSON string for C++ to parse
        var json = JSON.stringify(state);
        var len = lengthBytesUTF8(json) + 1;
        var buf = _malloc(len);
        stringToUTF8(json, buf, len);
        window._onState(buf);
        _free(buf);
      }
    });
  },

  // Emit input to server
  SocketIO_SendInput: function(left, right, jump) {
    if (window._socket) {
      window._socket.emit('input', {
        left:  left  !== 0,
        right: right !== 0,
        jump:  jump  !== 0,
      });
    }
  },

  // Disconnect
  SocketIO_Disconnect: function() {
    if (window._socket) window._socket.disconnect();
  },

  // --- Callback setters (C++ function pointers) ---
  SocketIO_SetOnConnect: function(cb) {
    window._onConnect = function() { Runtime.dynCall('v', cb, []); };
  },

  SocketIO_SetOnAssigned: function(cb) {
    window._onAssigned = function(sid, idx) {
      var sidBuf = lengthBytesUTF8(sid) + 1;
      var ptr = _malloc(sidBuf);
      stringToUTF8(sid, ptr, sidBuf);
      Runtime.dynCall('vii', cb, [ptr, idx]);
      _free(ptr);
    };
  },

  SocketIO_SetOnPlayerJoined: function(cb) {
    window._onPlayerJoined = function(sid, idx, x, y) {
      var sidBuf = lengthBytesUTF8(sid) + 1;
      var ptr = _malloc(sidBuf);
      stringToUTF8(sid, ptr, sidBuf);
      Runtime.dynCall('viiff', cb, [ptr, idx, x, y]);
      _free(ptr);
    };
  },

  SocketIO_SetOnPlayerLeft: function(cb) {
    window._onPlayerLeft = function(sid) {
      var sidBuf = lengthBytesUTF8(sid) + 1;
      var ptr = _malloc(sidBuf);
      stringToUTF8(sid, ptr, sidBuf);
      Runtime.dynCall('vi', cb, [ptr]);
      _free(ptr);
    };
  },

  SocketIO_SetOnState: function(cb) {
    window._onState = function(buf) {
      Runtime.dynCall('vi', cb, [buf]);
    };
  },
});
