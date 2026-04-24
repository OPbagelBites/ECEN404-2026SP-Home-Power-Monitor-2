import 'package:flutter/material.dart';
import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_database/firebase_database.dart'; 
import 'package:fl_chart/fl_chart.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'dart:math' as math; 

class HistoryPage extends StatefulWidget {
  const HistoryPage({super.key});

  @override
  State<HistoryPage> createState() => _HistoryPageState();
}

class _HistoryPageState extends State<HistoryPage> {
  String _selectedPeriod = 'Day';
  bool _showCost = false;

  @override
  Widget build(BuildContext context) {
    final user = FirebaseAuth.instance.currentUser;
    if (user == null) return const Center(child: Text("Please log in."));

    final settingsStream = FirebaseFirestore.instance
        .collection('users').doc(user.uid).collection('settings').doc('energy_cost')
        .snapshots();

    final historyStream = FirebaseDatabase.instance
        .ref("devices/esp32-devkitc-01/streams/fw-esp-0-1-1-adcdebug/frames")
        .limitToLast(3000)
        .onValue;

    return StreamBuilder<DocumentSnapshot>(
      stream: settingsStream,
      builder: (context, settingsSnapshot) {
        double pricePerKwh = 0.0;
        if (settingsSnapshot.hasData && settingsSnapshot.data!.exists) {
          final data = settingsSnapshot.data!.data() as Map<String, dynamic>?;
          pricePerKwh = (data?['price_per_kwh'] as num?)?.toDouble() ?? 0.0;
        }

        return Column(
          children: [
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 12.0, horizontal: 16.0),
              child: Column(
                children: [
                  SizedBox(
                    width: double.infinity,
                    child: SegmentedButton<String>(
                      segments: const [
                        ButtonSegment(value: 'Day', label: Text('Day')),
                        ButtonSegment(value: 'Week', label: Text('Week')),
                        ButtonSegment(value: 'Month', label: Text('Month')),
                      ],
                      selected: {_selectedPeriod},
                      onSelectionChanged: (Set<String> newSelection) {
                        setState(() => _selectedPeriod = newSelection.first);
                      },
                      style: const ButtonStyle(visualDensity: VisualDensity.compact),
                    ),
                  ),
                  const SizedBox(height: 12),
                  
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceBetween,
                    children: [
                      Text(
                        "Total $_selectedPeriod Cost",
                        style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16),
                      ),
                      Row(
                        children: [
                          const Text("Show Cost", style: TextStyle(fontWeight: FontWeight.bold)),
                          const SizedBox(width: 8),
                          Switch(
                            value: _showCost,
                            activeColor: Theme.of(context).colorScheme.secondary,
                            onChanged: (value) {
                              if (value == true && pricePerKwh <= 0.0001) {
                                showDialog(
                                  context: context,
                                  builder: (context) => AlertDialog(
                                    title: const Text("Set Electricity Rate"),
                                    content: const Text("To see cost estimates, please enter your price per kWh in Settings."),
                                    actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text("OK"))],
                                  ),
                                );
                              } else {
                                setState(() => _showCost = value);
                              }
                            }, 
                          ),
                        ],
                      ),
                    ],
                  ),
                ],
              ),
            ),

            Expanded(
              child: StreamBuilder<DatabaseEvent>(
                stream: historyStream,
                builder: (context, snapshot) {
                  if (snapshot.connectionState == ConnectionState.waiting) {
                    return const Center(child: CircularProgressIndicator());
                  }
                  
                  if (!snapshot.hasData || snapshot.data!.snapshot.value == null) {
                    return Center(
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Icon(Icons.history_toggle_off, size: 48, color: Colors.grey.shade400),
                          const SizedBox(height: 16),
                          Text("Awaiting Historical Data", style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold, color: Colors.grey.shade600)),
                          const SizedBox(height: 8),
                          Text("No MCU frames found in database.", textAlign: TextAlign.center, style: TextStyle(color: Colors.grey.shade500)),
                        ],
                      ),
                    );
                  }

                  final dataMap = snapshot.data!.snapshot.value as Map<dynamic, dynamic>;
                  final frames = dataMap.values.toList();
                  
                  final bars = _processData(frames, pricePerKwh);

                  double totalCost = 0.0;
                  for (var group in bars) {
                    double val = group.barRods.first.toY;
                    if (val > 0) {
                      if (_showCost) {
                        totalCost += val; 
                      } else {
                        double hoursPerBucket = _selectedPeriod == 'Day' ? 1.0 : 24.0;
                        totalCost += (val / 1000.0) * pricePerKwh * hoursPerBucket;
                      }
                    }
                  }

                  double highestValue = 0.0;
                  for (var group in bars) {
                    for (var rod in group.barRods) {
                      if (rod.toY > highestValue) highestValue = rod.toY;
                    }
                  }
                  double maxY = highestValue > 0 ? (highestValue * 1.2) : (_showCost ? 2.0 : 100.0);

                  return SingleChildScrollView(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Padding(
                          padding: const EdgeInsets.symmetric(horizontal: 16.0, vertical: 0.0),
                          child: Text(
                            "\$${totalCost.toStringAsFixed(2)}",
                            style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 32, color: Colors.green),
                          ),
                        ),
                        const SizedBox(height: 20),

                        // CHART
                        SizedBox(
                          height: 250,
                          child: Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 16.0),
                            child: BarChart(
                              BarChartData(
                                alignment: BarChartAlignment.spaceAround,
                                maxY: maxY, 
                                minY: 0,
                                barGroups: bars,
                                gridData: const FlGridData(show: false),
                                borderData: FlBorderData(show: false),
                                barTouchData: BarTouchData(
                                  touchTooltipData: BarTouchTooltipData(
                                    getTooltipColor: (_) => Colors.black,
                                    getTooltipItem: (group, groupIndex, rod, rodIndex) {
                                      if (rod.toY == 0) return null;
                                      String text = _showCost 
                                          ? '\$${rod.toY.toStringAsFixed(2)}' 
                                          : '${rod.toY.round()} W'; 
                                      return BarTooltipItem(text, const TextStyle(color: Colors.white, fontWeight: FontWeight.bold));
                                    },
                                  ),
                                ),
                                titlesData: FlTitlesData(
                                  leftTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                                  rightTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                                  topTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                                  bottomTitles: AxisTitles(
                                    sideTitles: SideTitles(
                                      showTitles: true,
                                      getTitlesWidget: (value, meta) => _getBottomTitles(value, meta),
                                    ),
                                  ),
                                ),
                              ),
                            ),
                          ),
                        ),

                        const Padding(padding: EdgeInsets.symmetric(vertical: 20), child: Divider()),
                        
                        // APPLIANCE BREAKDOWN
                        _buildDynamicUsageBreakdown(frames),
                        const SizedBox(height: 20),
                      ],
                    ),
                  );
                },
              ),
            ),
          ],
        );
      }
    );
  }

  // --- EXTRACT REAL TIMESTAMP FROM ML DATA ---
  int _extractTimestamp(dynamic frame) {
    if (frame['ml'] != null) {
      if (frame['ml']['received_ts'] != null) return (frame['ml']['received_ts'] as num).toInt();
      if (frame['ml']['ts'] != null) return (frame['ml']['ts'] as num).toInt();
    }
    return 0;
  }

  // --- TRUE CHRONOLOGICAL CHART BUCKETING ---
  List<BarChartGroupData> _processData(List<dynamic> frames, double price) {
    List<BarChartGroupData> groups = [];
    if (frames.isEmpty) return groups;

    double convert(double va) {
      if (!_showCost) return va;
      double hoursPerBucket = _selectedPeriod == 'Day' ? 1.0 : 24.0;
      return (va / 1000.0) * price * hoursPerBucket;
    }

    // Filter valid frames and find the latest recorded time
    List<Map<String, dynamic>> validData = [];
    int maxTs = 0;
    for (var f in frames) {
      if (f['s'] == null) continue; // Continuing to use 's' (VA) for UI stability
      int ts = _extractTimestamp(f);
      if (ts > 0) {
        // Ensure milliseconds
        if (ts < 10000000000) ts *= 1000;
        validData.add({'s': math.max(0.0, (f['s'] as num).toDouble()), 'ts': ts});
        if (ts > maxTs) maxTs = ts;
      }
    }

    if (validData.isEmpty) return groups;
    DateTime referenceTime = DateTime.fromMillisecondsSinceEpoch(maxTs);

    if (_selectedPeriod == 'Day') {
      DateTime startOfDay = DateTime(referenceTime.year, referenceTime.month, referenceTime.day);
      List<List<double>> hourlyBuckets = List.generate(24, (_) => []);

      for (var data in validData) {
        DateTime t = DateTime.fromMillisecondsSinceEpoch(data['ts']);
        if (t.isAfter(startOfDay) || t.isAtSameMomentAs(startOfDay)) {
          hourlyBuckets[t.hour].add(data['s']);
        }
      }

      for (int i = 0; i < 24; i++) {
        double val = 0;
        if (hourlyBuckets[i].isNotEmpty) {
           val = hourlyBuckets[i].reduce((a, b) => a + b) / hourlyBuckets[i].length;
        }
        groups.add(_makeBar(i, convert(val), width: 8, color: _showCost ? Colors.green : Colors.deepPurple));
      }

    } else if (_selectedPeriod == 'Week') {
      DateTime monday = referenceTime.subtract(Duration(days: referenceTime.weekday - 1));
      DateTime startOfWeek = DateTime(monday.year, monday.month, monday.day);
      List<List<double>> dailyBuckets = List.generate(7, (_) => []);

      for (var data in validData) {
        DateTime t = DateTime.fromMillisecondsSinceEpoch(data['ts']);
        if (t.isAfter(startOfWeek) || t.isAtSameMomentAs(startOfWeek)) {
          dailyBuckets[t.weekday - 1].add(data['s']);
        }
      }

      for (int i = 0; i < 7; i++) {
        double val = 0;
        if (dailyBuckets[i].isNotEmpty) {
           val = dailyBuckets[i].reduce((a, b) => a + b) / dailyBuckets[i].length;
        }
        groups.add(_makeBar(i, convert(val), width: 20, color: _showCost ? Colors.green : Colors.blue));
      }

    } else if (_selectedPeriod == 'Month') {
      DateTime startOfMonth = DateTime(referenceTime.year, referenceTime.month, 1);
      int daysInMonth = DateTime(referenceTime.year, referenceTime.month + 1, 0).day;
      List<List<double>> dayBuckets = List.generate(daysInMonth, (_) => []);

      for (var data in validData) {
        DateTime t = DateTime.fromMillisecondsSinceEpoch(data['ts']);
        if (t.isAfter(startOfMonth) || t.isAtSameMomentAs(startOfMonth)) {
          dayBuckets[t.day - 1].add(data['s']);
        }
      }

      for (int i = 0; i < daysInMonth; i++) {
        double val = 0;
        if (dayBuckets[i].isNotEmpty) {
           val = dayBuckets[i].reduce((a, b) => a + b) / dayBuckets[i].length;
        }
        groups.add(_makeBar(i, convert(val), width: 6, color: _showCost ? Colors.green : Colors.orange));
      }
    }

    return groups;
  }

  BarChartGroupData _makeBar(int x, double y, {Color color = Colors.deepPurple, double width = 8}) {
    return BarChartGroupData(
      x: x,
      barRods: [
        BarChartRodData(
          toY: y,
          color: color,
          width: width,
          borderRadius: BorderRadius.circular(2),
        )
      ],
    );
  }

  Widget _getBottomTitles(double value, TitleMeta meta) {
    int index = value.toInt();
    if (_selectedPeriod == 'Day') {
      if (index == 0) return const Text('12am', style: TextStyle(fontSize: 10));
      if (index == 6) return const Text('6am', style: TextStyle(fontSize: 10));
      if (index == 12) return const Text('Noon', style: TextStyle(fontSize: 10));
      if (index == 18) return const Text('6pm', style: TextStyle(fontSize: 10));
      if (index == 23) return const Text('11pm', style: TextStyle(fontSize: 10)); 
    } 
    else if (_selectedPeriod == 'Week') {
      const days = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
      if (index >= 0 && index < 7) {
        return Padding(padding: const EdgeInsets.only(top: 8.0), child: Text(days[index], style: const TextStyle(fontSize: 10)));
      }
    }
    else if (_selectedPeriod == 'Month') {
      if (index == 0) return const Text('1st', style: TextStyle(fontSize: 10));
      if (index == 7) return const Text('8th', style: TextStyle(fontSize: 10));
      if (index == 14) return const Text('15th', style: TextStyle(fontSize: 10));
      if (index == 21) return const Text('22nd', style: TextStyle(fontSize: 10));
      if (index == 29) return const Text('30th', style: TextStyle(fontSize: 10));
    }
    return const SizedBox();
  }

  Widget _buildDynamicUsageBreakdown(List<dynamic> frames) {
    Map<String, int> deviceCounts = {};
    int totalValidPredictions = 0; 

    for (var frame in frames) {
      if (frame['ml'] != null && frame['ml']['pred'] != null) {
        var preds = frame['ml']['pred'];
        if (preds is List && preds.isNotEmpty) {
          for (var pred in preds) {
            String deviceName = pred.toString();
            if (deviceName.toLowerCase() != 'off' && deviceName.toLowerCase() != 'none') {
                deviceCounts[deviceName] = (deviceCounts[deviceName] ?? 0) + 1;
                totalValidPredictions++; 
            }
          }
        } else if (preds is String && preds.isNotEmpty) {
           String deviceName = preds.toString();
           if (deviceName.toLowerCase() != 'off' && deviceName.toLowerCase() != 'none') {
               deviceCounts[deviceName] = (deviceCounts[deviceName] ?? 0) + 1;
               totalValidPredictions++;
           }
        }
      }
    }

    if (totalValidPredictions == 0) {
       return const Center(child: Text("No active appliances recognized."));
    }

    List<Map<String, dynamic>> devices = [];
    deviceCounts.forEach((name, count) {
      devices.add({
        'name': name,
        'percent': ((count / totalValidPredictions) * 100).round(), 
        'color': _getDeviceColor(name),
      });
    });

    devices.sort((a, b) => (b['percent'] as int).compareTo(a['percent'] as int));

    return ListView.builder(
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      itemCount: devices.length,
      padding: const EdgeInsets.symmetric(horizontal: 16),
      itemBuilder: (context, index) {
        final device = devices[index];
        return Card(
          margin: const EdgeInsets.only(bottom: 8),
          elevation: 0,
          color: Colors.grey.shade50,
          child: ListTile(
            leading: CircleAvatar(backgroundColor: (device['color'] as Color).withOpacity(0.2), child: Icon(Icons.flash_on, color: device['color'] as Color, size: 18)),
            title: Text(device['name'] as String, style: const TextStyle(fontWeight: FontWeight.w600)),
            trailing: Text("${device['percent']}%", style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
            subtitle: LinearProgressIndicator(value: (device['percent'] as int) / 100, backgroundColor: Colors.grey.shade200, color: device['color'] as Color, minHeight: 6, borderRadius: BorderRadius.circular(4)),
          ),
        );
      },
    );
  }

  Color _getDeviceColor(String name) {
    switch (name.toLowerCase()) {
      case 'hvac': return Colors.redAccent;
      case 'heater': return Colors.deepOrange;
      case 'water heater': return Colors.orangeAccent;
      case 'hairdryer': return Colors.pinkAccent;
      case 'curling iron': return Colors.purpleAccent; 
      case 'microwave': return Colors.blue;
      case 'fridge':
      case 'refrigerator': return Colors.green;
      case 'laptop':
      case 'electronics': return Colors.purple;
      case 'fan': return Colors.cyan;
      case 'light':
      case 'incandescent light bulb': 
      case 'lighting': return Colors.yellow.shade700;
      default: return Colors.grey;
    }
  }
}