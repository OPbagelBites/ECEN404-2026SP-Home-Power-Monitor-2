import 'package:flutter/material.dart';
import 'package:firebase_database/firebase_database.dart';
import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';

class LivePage extends StatefulWidget {
  const LivePage({super.key});

  @override
  State<LivePage> createState() => _LivePageState();
}

class _LivePageState extends State<LivePage> {
  @override
  Widget build(BuildContext context) {
    final user = FirebaseAuth.instance.currentUser;
    if (user == null) return const Center(child: Text("Please log in."));

    final settingsStream = FirebaseFirestore.instance
        .collection('users').doc(user.uid).collection('settings').doc('energy_cost')
        .snapshots();

    final liveStream = FirebaseDatabase.instance
        .ref("devices/esp32-devkitc-01/streams/fw-esp-0-1-1-adcdebug/frames")
        .limitToLast(5)
        .onValue;

    return StreamBuilder<DocumentSnapshot>(
      stream: settingsStream,
      builder: (context, settingsSnapshot) {
        double pricePerKwh = 0.0;
        if (settingsSnapshot.hasData && settingsSnapshot.data!.exists) {
          final data = settingsSnapshot.data!.data() as Map<String, dynamic>?;
          pricePerKwh = (data?['price_per_kwh'] as num?)?.toDouble() ?? 0.0;
        }

        return StreamBuilder<DatabaseEvent>(
          stream: liveStream,
          builder: (context, snapshot) {
            if (snapshot.connectionState == ConnectionState.waiting) {
              return const Center(child: CircularProgressIndicator());
            }

            if (!snapshot.hasData || snapshot.data!.snapshot.value == null) {
              return _buildEmptyState();
            }

            final dataMap = snapshot.data!.snapshot.value as Map<dynamic, dynamic>;
            List<dynamic> frames = dataMap.values.toList();
            frames.sort((a, b) => _extractTimestamp(a).compareTo(_extractTimestamp(b)));

            dynamic newestFrame = frames.last;
            
            // INSTANT METRICS
            double apparentPower = (newestFrame['s'] as num?)?.toDouble() ?? 0.0;
            double voltage = (newestFrame['rms_v'] as num?)?.toDouble() ?? 0.0;
            double current = (newestFrame['rms_i'] as num?)?.toDouble() ?? 0.0;

            // SMOOTHED COST LOGIC
            double totalBufferPower = 0.0;
            for (var f in frames) {
              totalBufferPower += (f['s'] as num?)?.toDouble() ?? 0.0;
            }
            double avgBufferPower = totalBufferPower / frames.length;
            double estCostPerHour = (avgBufferPower / 1000.0) * pricePerKwh;

            // --- NEW: MULTI-DEVICE MAJORITY VOTE ---
            Map<String, int> predictionCounts = {};
            for (var f in frames) {
              if (f['ml'] != null && f['ml']['pred'] != null) {
                var preds = f['ml']['pred'];
                // This safely handles if the ML outputs a List like: ["Hairdryer", "Light"]
                if (preds is List && preds.isNotEmpty) {
                  for (var p in preds) {
                    predictionCounts[p.toString()] = (predictionCounts[p.toString()] ?? 0) + 1;
                  }
                } else if (preds is String && preds.isNotEmpty) {
                  predictionCounts[preds] = (predictionCounts[preds] ?? 0) + 1;
                }
              }
            }

            // Keep any appliance that shows up in at least 3 out of the 10 frames
            List<String> stableAppliances = [];
            predictionCounts.forEach((appliance, votes) {
              // Ignore "off" or "none" strings just in case the ML throws those
              if (votes >= 2 && appliance.toLowerCase() != 'off' && appliance.toLowerCase() != 'none') {
                stableAppliances.add(appliance);
              }
            });

            // Fallback if nothing passed the 3-frame threshold
            if (stableAppliances.isEmpty) {
              stableAppliances.add("Unknown");
            }

            return SingleChildScrollView(
              padding: const EdgeInsets.all(16.0),
              child: Column(
                children: [
                  Card(
                    elevation: 0,
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16), side: BorderSide(color: Colors.grey.shade200)),
                    child: Padding(
                      padding: const EdgeInsets.all(24.0),
                      child: Column(
                        children: [
                          const Text("Live Consumption", style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
                          const SizedBox(height: 16),
                          Row(
                            mainAxisAlignment: MainAxisAlignment.center,
                            crossAxisAlignment: CrossAxisAlignment.baseline,
                            textBaseline: TextBaseline.alphabetic,
                            children: [
                              Text(apparentPower.round().toString(), style: const TextStyle(fontSize: 64, fontWeight: FontWeight.w500)),
                              const SizedBox(width: 8),
                              const Text("W", style: TextStyle(fontSize: 20, color: Colors.grey)), 
                            ],
                          ),
                          Text("At \$${pricePerKwh.toStringAsFixed(2)}/kWh", style: const TextStyle(color: Colors.grey)),
                          const Padding(padding: EdgeInsets.symmetric(vertical: 16.0), child: Divider()),
                          
                          _buildMetricRow(Icons.bolt, "Voltage", "${voltage.toStringAsFixed(1)} V", Colors.black),
                          const SizedBox(height: 12),
                          _buildMetricRow(Icons.power, "Current", "${current.toStringAsFixed(1)} A", Colors.black),
                          const SizedBox(height: 12),
                          _buildMetricRow(Icons.attach_money, "Est. Cost", "\$${estCostPerHour.toStringAsFixed(3)} / hr", Colors.green),
                        ],
                      ),
                    ),
                  ),
                  const SizedBox(height: 16),

                  Card(
                    elevation: 0,
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16), side: BorderSide(color: Colors.grey.shade200)),
                    child: Padding(
                      padding: const EdgeInsets.all(20.0),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text("Detected Appliances", style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
                          const SizedBox(height: 16),
                          
                          // --- NEW: DYNAMICALLY LIST ALL STABLE APPLIANCES ---
                          ...stableAppliances.map((appliance) {
                            return Padding(
                              padding: const EdgeInsets.only(bottom: 8.0), // Adds a tiny gap between devices
                              child: ListTile(
                                contentPadding: EdgeInsets.zero,
                                leading: Icon(_getIconFor(appliance), color: Colors.green),
                                title: Text(appliance, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w500)),
                                trailing: apparentPower > 5 
                                    ? const Text("Running", style: TextStyle(color: Colors.green, fontWeight: FontWeight.bold)) 
                                    : const Text("Standby", style: TextStyle(color: Colors.grey)),
                              ),
                            );
                          }),
                        ],
                      ),
                    ),
                  )
                ],
              ),
            );
          },
        );
      }
    );
  }

  Widget _buildMetricRow(IconData icon, String label, String value, Color valueColor) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Row(
          children: [
            Icon(icon, size: 20, color: Colors.grey.shade800),
            const SizedBox(width: 12),
            Text(label, style: const TextStyle(fontSize: 15)),
          ],
        ),
        Text(value, style: TextStyle(fontSize: 15, fontWeight: FontWeight.bold, color: valueColor)),
      ],
    );
  }

  Widget _buildEmptyState() {
    return const Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(Icons.power_off, size: 48, color: Colors.grey),
          SizedBox(height: 16),
          Text("Waiting for MCU...", style: TextStyle(fontSize: 18, color: Colors.grey)),
        ],
      ),
    );
  }

  int _extractTimestamp(dynamic frame) {
    if (frame['t'] != null) return (frame['t'] as num).toInt();
    if (frame['ml'] != null && frame['ml']['ts'] != null) return (frame['ml']['ts'] as num).toInt();
    return 0;
  }

  IconData _getIconFor(String name) {
    switch (name.toLowerCase()) {
      case 'hvac': return Icons.ac_unit;
      case 'heater': return Icons.local_fire_department;
      case 'hairdryer': return Icons.air;
      case 'curling iron': return Icons.face_retouching_natural;
      case 'microwave': return Icons.microwave;
      case 'fridge': return Icons.kitchen;
      case 'laptop': return Icons.laptop;
      case 'fan': return Icons.mode_fan_off;
      case 'light': return Icons.lightbulb;
      default: return Icons.power;
    }
  }
}