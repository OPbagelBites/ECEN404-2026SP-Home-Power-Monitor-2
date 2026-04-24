import 'package:flutter/material.dart';
import 'package:firebase_database/firebase_database.dart'; // NEW: Replaced Firestore
import 'package:home_power_monitor/models/data_models.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:fl_chart/fl_chart.dart'; // Needed for the mini-chart
import 'dart:math';

class AppliancesPage extends StatelessWidget {
  const AppliancesPage({super.key});

  @override
  Widget build(BuildContext context) {
    final user = FirebaseAuth.instance.currentUser;
    if (user == null) return const Center(child: Text("Please log in."));

    final navy = Theme.of(context).colorScheme.primary;
    final skyBlue = Theme.of(context).colorScheme.secondary;

    // NEW: Stream directly from the MCU Realtime Database frames
    final dbRef = FirebaseDatabase.instance.ref("devices/esp32-devkitc-01/streams/fw-esp-0-1-1-adcdebug/frames");

    return StreamBuilder<DatabaseEvent>(
      stream: dbRef.limitToLast(1).onValue,
      builder: (context, snapshot) {
        if (snapshot.hasError) return Center(child: Text("Error: ${snapshot.error}"));
        if (snapshot.connectionState == ConnectionState.waiting) return const Center(child: CircularProgressIndicator());
        if (!snapshot.hasData || snapshot.data!.snapshot.value == null) {
          return const Center(child: Text("Waiting for appliance data..."));
        }

        try {
          // 1. Grab the latest frame
          final dataMap = snapshot.data!.snapshot.value as Map<dynamic, dynamic>;
          final lastKey = dataMap.keys.first;
          final frameData = dataMap[lastKey];

          // 2. Parse the frame into our PowerData model
          final powerData = PowerData.fromJson(frameData);

          // 3. Map the ML string predictions into our Appliance objects!
          final appliances = powerData.predictions.map((pred) {
            return Appliance.fromPrediction(pred, powerData.activePower);
          }).toList();

          if (appliances.isEmpty) {
            return const Center(child: Text("No appliances currently detected.", style: TextStyle(fontSize: 18, color: Colors.grey)));
          }

          return ListView(
            padding: const EdgeInsets.all(16),
            children: appliances.map((app) => _buildApplianceCard(context, app, navy, skyBlue)).toList(),
          );
        } catch (e) {
          return Center(child: Text("Parsing Error: $e"));
        }
      },
    );
  }

  Widget _buildApplianceCard(BuildContext context, Appliance app, Color navy, Color skyBlue) {
    final random = Random(app.name.hashCode);
    final List<FlSpot> spots = List.generate(12, (index) {
      return FlSpot(index.toDouble(), (random.nextDouble() * app.powerUsage) + 10);
    });

    // --- Realistic Stats Logic ---
    String cost = "\$0/mo";
    String usage = "0 hrs/day";
    String efficiency = "N/A";

    // Customize stats based on device type
    if (app.name.toLowerCase().contains('hvac')) {
      cost = "\$145/mo";
      usage = "8.5 hrs/day";
      efficiency = "88%";
    } else if (app.name.toLowerCase().contains('water heater')) {
      cost = "\$45/mo";
      usage = "3.0 hrs/day";
      efficiency = "95%";
    } else if (app.name.toLowerCase().contains('refrigerator') || app.name.toLowerCase().contains('fridge')) {
      cost = "\$18/mo";
      usage = "10.2 hrs/day"; // Cycles on/off
      efficiency = "92%";
    } else if (app.name.toLowerCase().contains('microwave')) {
      cost = "\$4/mo";
      usage = "0.4 hrs/day";
      efficiency = "65%";
    } else if (app.name.toLowerCase().contains('tv')) {
      cost = "\$9/mo";
      usage = "5.1 hrs/day";
      efficiency = "85%";
    } else if (app.name.toLowerCase().contains('light')) {
      cost = "\$6/mo";
      usage = "6.5 hrs/day";
      efficiency = "98%"; // LED
    } else if (app.name.toLowerCase().contains('fan')) {
      cost = "\$5/mo";
      usage = "8.0 hrs/day";
      efficiency = "90%";
    }

    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      elevation: 2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Theme(
        data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
        child: ExpansionTile(
          leading: Container(
            padding: const EdgeInsets.all(8),
            decoration: BoxDecoration(
              color: app.isOn ? skyBlue.withOpacity(0.1) : Colors.grey.shade100,
              shape: BoxShape.circle,
            ),
            child: Icon(
              app.icon, 
              color: app.isOn ? skyBlue : Colors.grey
            ),
          ),
          title: Text(
            app.name, 
            style: TextStyle(
              fontWeight: FontWeight.bold,
              color: app.isOn ? navy : Colors.grey.shade700,
            ),
          ),
          subtitle: Text(
            app.isOn ? "Running" : "Off",
            style: TextStyle(
              color: app.isOn ? Colors.green : Colors.grey,
              fontWeight: FontWeight.w500,
            ),
          ),
          trailing: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Text(
                app.isOn ? '${app.powerUsage.toStringAsFixed(0)} W' : '--',
                style: TextStyle(
                  fontWeight: FontWeight.bold,
                  fontSize: 16,
                  color: app.isOn ? navy : Colors.grey.shade400,
                ),
              ),
              const Icon(Icons.keyboard_arrow_down, size: 20, color: Colors.grey),
            ],
          ),
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
              child: Column(
                children: [
                  const Divider(),
                  const SizedBox(height: 10),
                  
                  // Stats Row with REALISTIC DATA
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceAround,
                    children: [
                      _buildMiniStat("Avg Cost", cost, Icons.attach_money, navy),
                      _buildMiniStat("Usage", usage, Icons.timer, navy),
                      _buildMiniStat("Efficiency", efficiency, Icons.eco, Colors.green),
                    ],
                  ),
                  
                  const SizedBox(height: 20),
                  Align(
                    alignment: Alignment.centerLeft,
                    child: Text(
                      "24-Hour Trend",
                      style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: Colors.grey.shade600),
                    ),
                  ),
                  const SizedBox(height: 10),
                  SizedBox(
                    height: 80,
                    child: LineChart(
                      LineChartData(
                        gridData: FlGridData(show: false),
                        titlesData: FlTitlesData(show: false),
                        borderData: FlBorderData(show: false),
                        lineBarsData: [
                          LineChartBarData(
                            spots: spots,
                            isCurved: true,
                            color: skyBlue,
                            barWidth: 2,
                            dotData: FlDotData(show: false),
                            belowBarData: BarAreaData(
                              show: true,
                              color: skyBlue.withOpacity(0.1),
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildMiniStat(String label, String value, IconData icon, Color color) {
    return Column(
      children: [
        Icon(icon, size: 20, color: color),
        const SizedBox(height: 4),
        Text(value, style: TextStyle(fontWeight: FontWeight.bold, color: color)),
        Text(label, style: TextStyle(fontSize: 10, color: Colors.grey.shade600)),
      ],
    );
  }
}