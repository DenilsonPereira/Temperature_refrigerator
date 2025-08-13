const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const mqtt = require('mqtt');
const path = require('path');
const { MongoClient } = require('mongodb');

// Configurações do broker e do banco de dados
const MQTT_BROKER_URL = "mqtt://localhost:1883";
const MQTT_TOPIC_TEMP = "sensor/pt100/temperatura";
const MONGO_CONNECTION_STRING = "mongodb://localhost:27017/";
const MONGO_DB_NAME = "iot_sensores";
const MONGO_COLLECTION_NAME = "temperatura_pt100";
const WEB_SERVER_PORT = 3000;

let dbCollection;
const app = express();
app.use(express.static(path.join(__dirname)));
const server = http.createServer(app);

// Rotas das api
app.get('/api/stats', async (req, res) => {
    if (!dbCollection) return res.status(503).send('MongoDB não conectado');
    try {
        const stats = await dbCollection.aggregate([{ $group: { _id: null, minTemp: { $min: "$temperatura" }, maxTemp: { $max: "$temperatura" } } }]).toArray();
        res.json(stats[0] || { minTemp: null, maxTemp: null });
    } catch (e) {
        res.status(500).send(`Erro ao buscar estatísticas: ${e}`);
    }
});

app.get('/api/history/day', async (req, res) => {
    if (!dbCollection) return res.status(503).send('MongoDB não conectado');
    try {
        const oneDayAgo = new Date(Date.now() - 24 * 60 * 60 * 1000);
        const history = await dbCollection.find({ timestamp: { $gte: oneDayAgo } })
            .sort({ timestamp: -1 }).limit(2000).sort({ timestamp: 1 }).toArray();
        res.json(history);
    } catch (e) {
        res.status(500).send(`Erro ao buscar histórico: ${e}`);
    }
});

app.get('/api/report', async (req, res) => {
    if (!dbCollection) return res.status(503).send('MongoDB não conectado');
    try {
        const now = new Date();
        const startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 0, 0, 0);

        const time8am = new Date(startOfDay); time8am.setHours(8);
        const time12pm = new Date(startOfDay); time12pm.setHours(12);
        const time6pm = new Date(startOfDay); time6pm.setHours(18);
        const endOfDay = new Date(startOfDay); endOfDay.setDate(startOfDay.getDate() + 1);

        const findFirstReading = async (startTime, endTime) => {
            const query = { timestamp: { $gte: startTime, $lt: endTime } };
            return await dbCollection.findOne(query, { sort: { timestamp: 1 } });
        };

        const report8am = await findFirstReading(time8am, time12pm);
        const report12pm = await findFirstReading(time12pm, time6pm);
        const report6pm = await findFirstReading(time6pm, endOfDay);

        res.json({
            t8am: report8am ? report8am.temperatura : null,
            t12pm: report12pm ? report12pm.temperatura : null,
            t6pm: report6pm ? report6pm.temperatura : null,
        });

    } catch (e) {
        res.status(500).send(`Erro ao buscar relatório: ${e}`);
    }
});

const wss = new WebSocket.Server({ server });
wss.on('connection', (ws) => console.log('Cliente web conectado.'));
function broadcast(data) { wss.clients.forEach((client) => { if (client.readyState === WebSocket.OPEN) client.send(data); }); }

const mqttClient = mqtt.connect(MQTT_BROKER_URL);
mqttClient.on('connect', () => {
    console.log('Conectado ao Broker MQTT!');
    mqttClient.subscribe(MQTT_TOPIC_TEMP);
});

mqttClient.on('message', (topic, payload) => {
    const message = payload.toString();
    console.log(`Recebido do MQTT [Temp]: ${message}`);
    broadcast(message);
    try {
        const temperaturaFloat = parseFloat(message);
        if (!isNaN(temperaturaFloat)) {
            dbCollection.insertOne({ sensor_id: "PT100_Geladeira_01", temperatura: temperaturaFloat, unidade: "C", timestamp: new Date() });
            console.log(` -> Salvo no MongoDB: ${temperaturaFloat}°C`);
        }
    } catch (e) { console.error(`Erro ao salvar no MongoDB: ${e}`); }
});

async function startServer() {
    const mongoClient = new MongoClient(MONGO_CONNECTION_STRING);
    await mongoClient.connect();
    dbCollection = mongoClient.db(MONGO_DB_NAME).collection(MONGO_COLLECTION_NAME);
    console.log("Conectado ao MongoDB!");
    server.listen(WEB_SERVER_PORT, () => {
        console.log(`Dashboard rodando em http://localhost:${WEB_SERVER_PORT}`);
    });
}

startServer();