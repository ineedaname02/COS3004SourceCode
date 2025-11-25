const { onDocumentCreated } = require("firebase-functions/v2/firestore");
const { onCall } = require("firebase-functions/v2/https");
const { defineSecret } = require("firebase-functions/params");
const admin = require("firebase-admin");
const OpenAI = require("openai");
const crypto = require('crypto');

admin.initializeApp();
const db = admin.firestore();

// 🔐 Define secrets (use Firebase CLI: firebase functions:secrets:set ADMIN_KEY)

const openaiKey = defineSecret("OPENAI_KEY");
const adminKeySecret = defineSecret("ENCRYPTION_ADMIN_KEY");

exports.getDataKey = onCall({
  region: "asia-southeast1",
  enforceAppCheck: false,
  cors: true,
  secrets: [adminKeySecret], // ✅ Declare the secret here
}, async (request) => {
  
  function getEncryptionKey() {
    const key = adminKeySecret.value();
    if (!key) {
      throw new Error('Admin encryption key not configured.');
    }
    return key;
  }

  console.log('🔑 getDataKey called by user:', request.auth?.uid);
  
  if (!request.auth) {
    console.log('❌ No authentication in request');
    throw new functions.https.HttpsError('unauthenticated', 'User must be authenticated');
  }

  const userId = request.auth.uid;
  
  try {
    console.log('👤 Checking user profile for:', userId);
    
    const userDoc = await admin.firestore()
      .collection('userProfiles')
      .doc(userId)
      .get();

    console.log('📄 User document exists:', userDoc.exists);
    
    if (!userDoc.exists) {
      throw new functions.https.HttpsError('permission-denied', 'User profile not found');
    }

    const userData = userDoc.data();
    console.log('🔍 User role:', userData?.role);
    
    if (userData.role !== 'admin') {
      throw new functions.https.HttpsError('permission-denied', 'Only admins can access encryption keys');
    }

    console.log('✅ User is admin, generating encryption key...');
    const encryptionKey = getEncryptionKey();
    
    const dataKey = crypto.createHash('sha256')
      .update(encryptionKey + "endangered_data_key")
      .digest('hex')
      .substring(0, 32);

    console.log('✅ Encryption key generated for user:', userId);
    
    return {
      key: dataKey,
      timestamp: admin.firestore.Timestamp.now(),
      userId: userId
    };

  } catch (error) {
    console.error('❌ Error in getDataKey:', error);
    throw new functions.https.HttpsError('internal', 'Internal server error');
  }
});


// ✅ Firestore trigger for event notifications
exports.newEventNotification = onDocumentCreated(
  {
    document: "events/{eventId}",
    region: "asia-southeast1",
  },
  async (event) => {
    console.log(" FIRESTORE TRIGGER ACTIVATED!");
    console.log(" Document ID:", event.params.eventId);
    
    const data = event.data?.data();
    console.log(" Document data:", JSON.stringify(data, null, 2));
    
    if (!data) {
      console.log(" No data found in document");
      return null;
    }

    const deviceId = data.deviceId || "Unknown device";
    const message = data.message || "New event detected";
    const priority = data.priority || "medium";
    const type = data.type || "unknown";

    console.log(`Creating notification:`, { deviceId, message, priority, type });

    try {
      const payload = {
        notification: {
          title: `🔔 ${type.toUpperCase()} from ${deviceId}`,
          body: `${message} (Priority: ${priority})`,
        },
        data: {
          deviceId: deviceId,
          message: message,
          priority: priority,
          type: type,
          value: data.value?.toString() || "",
          timestamp: data.timestamp || "",
          eventId: event.params.eventId,
        },
        topic: "admins"
      };

      console.log("📤 Sending to FCM topic: admins");
      
      // Send notification
      const response = await admin.messaging().send(payload);
      
      console.log("✅ FCM Notification sent successfully!");
      console.log("📋 FCM Response:", response);
      
      return response;
    } catch (error) {
      console.error("❌ FCM Error:", error.message);
      
      // Fallback: Try without topic (condition)
      try {
        console.log("🔄 Trying fallback FCM approach...");
        const fallbackPayload = {
          notification: {
            title: `🔔 ${type.toUpperCase()} from ${deviceId}`,
            body: `${message} (Priority: ${priority})`,
          },
          condition: "'admins' in topics"
        };
        
        const fallbackResponse = await admin.messaging().send(fallbackPayload);
        console.log("✅ Fallback FCM worked!");
        return fallbackResponse;
      } catch (fallbackError) {
        console.error("❌ Fallback FCM also failed:", fallbackError.message);
        return null;
      }
    }
  }
);

// ✅ Callable GPT Agent
exports.queryAgent = onCall(
  { 
    region: "asia-southeast1",
    secrets: [openaiKey],
  },
  async (request) => {
    const openai = new OpenAI({
      apiKey: openaiKey.value(),
    });

    const userPrompt = request.data?.prompt || "Hello, how can you help me?";
    const deviceId = request.data?.deviceId; // Optional device filter
    const timeRange = request.data?.timeRange || "latest"; // latest, today, week, month
    
    try {
      // Get device status information first
      let devices = {};
      try {
        const devicesSnapshot = await db.collection("devices").get();
        devicesSnapshot.forEach(doc => {
          devices[doc.id] = doc.data();
        });
      } catch (deviceError) {
        console.warn("⚠️ Could not fetch device status:", deviceError.message);
        // Continue without device status
      }

      // Build query for readings
      let query = db.collection("readings");
      
      // Filter by device if specified
      if (deviceId) {
        query = query.where("deviceId", "==", deviceId);
      }
      
      let contextMessage = '';
      let validReadings = [];

      if (timeRange === "latest") {
        // Get latest valid readings
        const snapshot = await query
          .orderBy("timestamp", "desc")
          .limit(deviceId ? 10 : 20) // Get more to filter out invalid timestamps
          .get();

        if (snapshot.empty) {
          return { reply: "No sensor readings found. Please check if your devices are connected." };
        }

        // Filter out readings with invalid timestamps
        validReadings = snapshot.docs
          .map(doc => ({
            id: doc.id,
            ...doc.data()
          }))
          .filter(reading => isValidTimestamp(reading.timestamp))
          .slice(0, deviceId ? 1 : 5); // Take only the most recent valid ones

        if (validReadings.length === 0) {
          return { reply: "No valid recent sensor readings found. Devices may be syncing." };
        }

        contextMessage = buildLatestContext(validReadings, devices, deviceId);

      } else {
        // Get historical data with valid timestamps
        const timeFilter = getTimeFilter(timeRange);
        const snapshot = await query
          .orderBy("timestamp", "desc")
          .limit(100) // Get more to filter
          .get();

        if (snapshot.empty) {
          return { reply: `No historical data found for the specified time range (${timeRange}).` };
        }

        // Filter for valid timestamps within time range
        validReadings = snapshot.docs
          .map(doc => ({
            id: doc.id,
            ...doc.data()
          }))
          .filter(reading => {
            if (!isValidTimestamp(reading.timestamp)) return false;
            const readingTime = new Date(reading.timestamp);
            return readingTime >= timeFilter;
          })
          .slice(0, 50); // Limit for context size

        if (validReadings.length === 0) {
          return { reply: `No valid historical data found for the ${timeRange} period.` };
        }

        contextMessage = buildHistoricalContext(validReadings, timeRange, devices, deviceId);
      }

      // Get recent events for additional context
      let recentEvents = [];
      try {
        const eventsSnapshot = await db.collection("events")
          .orderBy("timestamp", "desc")
          .limit(15)
          .get();

        recentEvents = eventsSnapshot.docs
          .map(doc => doc.data())
          .filter(event => isValidTimestamp(event.timestamp))
          .slice(0, 10);
      } catch (eventsError) {
        console.warn("⚠️ Could not fetch events:", eventsError.message);
        // Continue without events
      }

      if (recentEvents.length > 0) {
        contextMessage += `\n\nRecent System Events (last 10 valid events):\n${formatEvents(recentEvents)}`;
      }

      console.log("📊 Data loaded:", {
        validReadings: validReadings.length,
        events: recentEvents.length,
        devices: Object.keys(devices).length,
        deviceFilter: deviceId,
        timeRange: timeRange
      });

      const completion = await openai.chat.completions.create({
        model: "gpt-4o-mini",
        messages: [
          { role: "system", content: contextMessage },
          { role: "user", content: userPrompt },
        ],
        max_tokens: 600,
      });

      const reply = completion.choices[0].message.content;
      console.log("🤖 AI Response generated");
      return { reply };
    } catch (err) {
      console.error("❌ Error querying GPT agent:", err);
      return handleError(err, deviceId);
    }
  }
);

// Helper functions
function isValidTimestamp(timestamp) {
  if (!timestamp) return false;
  
  // Check if it's a string that can be parsed as a date
  if (typeof timestamp === 'string') {
    // Skip obviously wrong timestamps (1970 dates, numeric strings, etc.)
    if (timestamp.startsWith('1970-')) return false;
    if (/^\d+$/.test(timestamp)) return false; // Pure numbers like "51651"
    
    const date = new Date(timestamp);
    return !isNaN(date.getTime()) && date > new Date('2020-01-01');
  }
  
  // If it's a number, check if it's a reasonable Unix timestamp
  if (typeof timestamp === 'number') {
    const date = new Date(timestamp);
    return !isNaN(date.getTime()) && date > new Date('2020-01-01');
  }
  
  return false;
}

function getTimeFilter(timeRange) {
  const now = new Date();
  switch (timeRange) {
    case "today":
      return new Date(now.setHours(0, 0, 0, 0));
    case "week":
      return new Date(now.setDate(now.getDate() - 7));
    case "month":
      return new Date(now.setMonth(now.getMonth() - 1));
    default:
      return new Date('2020-01-01'); // Reasonable start date
  }
}

function buildLatestContext(readingsData, devices, deviceId) {
  const now = new Date();
  
  if (deviceId) {
    const reading = readingsData[0];
    const device = devices[deviceId];
    const deviceStatus = device ? 
      `Status: ${device.status}, Last Seen: ${formatTimeAgo(device.lastSeen)}` : 
      'Status: Unknown';
    
    return `
You are an IoT assistant for a smart agriculture system called myPlant.
Current time: ${now.toISOString()}

DEVICE INFORMATION:
- Device: ${deviceId}
- ${deviceStatus}

LATEST SENSOR READING (timestamp: ${reading.timestamp}):
${formatReading(reading)}

SENSOR INTERPRETATION GUIDE:
- moisture: Higher values (4000+) = drier soil, Lower values = wetter soil
- rain: 1 = raining, 0 = not raining
- humidity: Percentage (0-100%)
- temperature: Celsius
- lightDigital: Higher values = brighter light

Answer the user's question naturally and helpfully using this data.
If the user asks about sensor readings, explain what the values mean for plant health.
Be concise but friendly in your responses. Point out any concerning readings.
    `;
  } else {
    // Multiple devices context
    let devicesSummary = Object.values(devices).map(device => 
      `- ${device.name}: ${device.status}, Last: ${formatTimeAgo(device.lastSeen)}`
    ).join('\n');

    return `
You are an IoT assistant for a smart agriculture system called myPlant.
Current time: ${now.toISOString()}

DEVICES SUMMARY:
${devicesSummary}

LATEST READINGS FROM ALL DEVICES:
${readingsData.map(reading => 
  `Device ${reading.deviceId} (${formatTimeAgo(reading.timestamp)}):\n${formatReading(reading)}`
).join('\n\n')}

SENSOR INTERPRETATION GUIDE:
- moisture: Higher values (4000+) = drier soil, Lower values = wetter soil
- rain: 1 = raining, 0 = not raining
- humidity: Percentage (0-100%)
- temperature: Celsius  
- lightDigital: Higher values = brighter light

When answering questions:
- Specify which device's data you're referring to
- Compare readings between devices if relevant
- Help identify any devices that might need attention
- Explain what sensor values mean for plant health
- Be concise but friendly in your responses.
    `;
  }
}

function buildHistoricalContext(readingsData, timeRange, devices, deviceId) {
  const deviceContext = deviceId ? ` from device: ${deviceId}` : ' from all devices';
  
  return `
You are an IoT assistant for a smart agriculture system called myPlant.
Analyzing historical data${deviceContext} for the ${timeRange} period.

DEVICE STATUS:
${Object.values(devices).map(device => 
  `- ${device.name}: ${device.status}, Last Seen: ${formatTimeAgo(device.lastSeen)}`
).join('\n')}

HISTORICAL SENSOR READINGS (${readingsData.length} valid data points):
${readingsData.map(reading => 
  `[${reading.timestamp}] Device ${reading.deviceId}:\n${formatReading(reading)}`
).join('\n\n')}

SENSOR INTERPRETATION GUIDE:
- moisture: Higher values (4000+) = drier soil, Lower values = wetter soil
- rain: 1 = raining, 0 = not raining
- humidity: Percentage (0-100%)
- temperature: Celsius
- lightDigital: Higher values = brighter light

When answering questions about historical data:
- Identify trends and patterns over time
- Note any anomalies or concerning readings
- Provide insights about plant health and environmental conditions
- Compare current vs historical performance if relevant
- Explain what the data means for plant care
- Be concise but analytical in your responses.
  `;
}

function formatReading(reading) {
  const fields = [];
  if (reading.temperature !== undefined) fields.push(`Temperature: ${reading.temperature}°C`);
  if (reading.humidity !== undefined) fields.push(`Humidity: ${reading.humidity}%`);
  if (reading.moisture !== undefined) fields.push(`Soil Moisture: ${reading.moisture} (lower=wetter)`);
  if (reading.rain !== undefined) fields.push(`Rain: ${reading.rain === 1 ? 'Yes' : 'No'}`);
  if (reading.sound !== undefined) fields.push(`Sound: ${reading.sound}`);
  if (reading.lightAnalog !== undefined) fields.push(`Light Analog: ${reading.lightAnalog}`);
  if (reading.lightDigital !== undefined) fields.push(`Light Digital: ${reading.lightDigital}`);
  
  return fields.join(', ');
}

function formatEvents(events) {
  return events.map(event => 
    `[${event.timestamp}] ${event.deviceId}: ${event.type} - ${event.message} (${event.priority} priority)`
  ).join('\n');
}

function formatTimeAgo(timestamp) {
  if (!isValidTimestamp(timestamp)) return 'invalid time';
  
  const time = new Date(timestamp);
  const now = new Date();
  const diffMs = now - time;
  const diffMins = Math.floor(diffMs / (1000 * 60));
  const diffHours = Math.floor(diffMs / (1000 * 60 * 60));
  const diffDays = Math.floor(diffMs / (1000 * 60 * 60 * 24));
  
  if (diffMins < 60) return `${diffMins} minutes ago`;
  if (diffHours < 24) return `${diffHours} hours ago`;
  return `${diffDays} days ago`;
}

// ✅ ADD THIS MISSING FUNCTION
async function handleError(err, deviceId) {
  console.error("❌ Error in queryAgent:", err);
  
  // Handle specific OpenAI errors
  if (err.status === 429) {
    if (err.code === 'insufficient_quota') {
      return { 
        reply: "AI service is currently unavailable due to billing issues. Please check your OpenAI account billing settings." 
      };
    }
    return { 
      reply: "AI service is busy right now. Please try again in a moment." 
    };
  }
  
  if (err.status === 401) {
    return { 
      reply: "AI service configuration error. Please contact administrator." 
    };
  }
  
  // Provide fallback response with latest data
  try {
    let query = db.collection("readings").orderBy("timestamp", "desc");
    if (deviceId) {
      query = query.where("deviceId", "==", deviceId);
    }
    
    const snapshot = await query.limit(deviceId ? 1 : 3).get();

    if (!snapshot.empty) {
      const readings = snapshot.docs.map(doc => {
        const data = doc.data();
        return {
          device: data.deviceId || "unknown",
          temperature: data.temperature || data.temp || "unknown",
          humidity: data.humidity || "unknown",
          soilMoisture: data.soilMoisture || data.moisture || "unknown"
        };
      });
      
      return { 
        reply: `I'm currently having issues with my AI service. Here are the latest readings: ${JSON.stringify(readings)}` 
      };
    }
  } catch (fallbackError) {
    console.error("Fallback also failed:", fallbackError);
  }
  
  return { 
    reply: "Sorry, I'm having trouble responding right now. Please try again later." 
  };
}

// ✅ Test FCM function
exports.testFCM = onCall(
  { region: "asia-southeast1" },
  async (request) => {
    try {
      console.log("🧪 Testing FCM...");
      
      const testPayload = {
        notification: {
          title: "✅ Test Notification",
          body: "This is a test message from Firebase Functions",
        },
        topic: "admins"
      };
      
      const response = await admin.messaging().send(testPayload);
      console.log("✅ Test FCM Success:", response);
      return { 
        success: true, 
        message: "Test notification sent successfully!",
        response: response 
      };
      
    } catch (error) {
      console.error("❌ Test FCM Failed:", error);
      return { 
        success: false, 
        error: error.message,
        details: "Make sure FCM is enabled and devices are subscribed to 'admins' topic"
      };
    }
  }
);

// ✅ Simple hello world function for testing
exports.helloWorld = onCall(
  { region: "asia-southeast1" },
  async (request) => {
    return {
      message: "Hello from Firebase Functions!",
      timestamp: new Date().toISOString(),
      status: "working"
    };
  }
);