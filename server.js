const express = require('express');
const http = require('http');
const path = require('path');
const { Server } = require('socket.io');

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
    cors: { origin: "*" }
});

// 提供静态文件（receiver.html）
app.use(express.static(path.join(__dirname, 'public')));

// 信令逻辑
io.on('connection', (socket) => {
    console.log(`[连接] ${socket.id}`);

    socket.on('join', (room) => {
        socket.join(room);
        socket.to(room).emit('peer-joined', socket.id);
        console.log(`[加入房间] ${socket.id} → ${room}`);
    });

    socket.on('offer', (data) => {
        if (!data.room || !data.sdp) return;
        console.log(`[Offer] ${socket.id} → room: ${data.room}`);
        socket.to(data.room).emit('offer', {
            sdp: data.sdp,
            from: socket.id
        });
    });

    socket.on('answer', (data) => {
        if (!data.room || !data.sdp) return;
        console.log(`[Answer] ${socket.id} → room: ${data.room}`);
        socket.to(data.room).emit('answer', {
            sdp: data.sdp,
            from: socket.id
        });
    });

    socket.on('ice-candidate', (data) => {
        if (!data.room) return;
        socket.to(data.room).emit('ice-candidate', {
            candidate: data.candidate,
            from: socket.id
        });
    });

    socket.on('disconnect', () => {
        console.log(`[断开] ${socket.id}`);
    });
});

const PORT = 3000;
server.listen(PORT, '0.0.0.0', () => {
    console.log(`========================================`);
    console.log(`  信令服务器已启动: http://0.0.0.0:${PORT}`);
    console.log(`  接收端页面: http://本机IP:${PORT}/receiver.html`);
    console.log(`========================================`);
});