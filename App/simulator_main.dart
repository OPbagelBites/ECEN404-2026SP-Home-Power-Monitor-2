import 'dart:async';
import 'dart:math';
import 'package:firebase_core/firebase_core.dart';
import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:flutter/material.dart';
import 'firebase_options.dart';

// --- CONFIGURATION ---
String targetUserId = ""; 
// ---------------------

final Map<String, dynamic> _appliances = {
  'fridge': {'id': 'fridge', 'name': 'Refrigerator', 'icon_name': 'refrigerator', 'power_usage': 150.0, 'is_on': true},
  'tv': {'id': 'tv', 'name': 'Living Room TV', 'icon_name': 'tv', 'power_usage': 200.0, 'is_on': false},
  'microwave': {'id': 'microwave', 'name': 'Microwave', 'icon_name': 'microwave', 'power_usage': 1200.0, 'is_on': false},
  'hvac': {'id': 'hvac', 'name': 'HVAC', 'icon_name': 'hvac', 'power_usage': 3500.0, 'is_on': false},
  'lights': {'id': 'lights', 'name': 'Kitchen Lights', 'icon_name': 'lights', 'power_usage': 60.0, 'is_on': true},
};

// Variable to track "Ghost Power" (Base load) so it drifts smoothly
double _ghostPower = 400.0;

Future<void> main() async {
  print("--- Starting Smart Simulator ---");
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(options: DefaultFirebaseOptions.currentPlatform);
  
  final firestore = FirebaseFirestore.instance;
  final random = Random();
  double baseVoltage = 118.0;

  // 1. LISTEN TO SWITCHBOARD
  firestore.collection('config').doc('simulator_control').snapshots().listen((snapshot) {
    if (snapshot.exists) {
      final newId = snapshot.data()?['active_user_id'] as String?;
      if (newId != null && newId != targetUserId) {
        targetUserId = newId;
        print("\n>>> TARGET CHANGED! Now sending to: $targetUserId\n");
        _smartBackfillHistory(firestore, targetUserId);
      }
    }
  });

  // 2. LIVE DATA LOOP (2s)
  Timer.periodic(const Duration(seconds: 2), (timer) {
    if (targetUserId.isEmpty) { print("Waiting for user login..."); return; }

    // 1. Calculate Appliance Power (Jumps are okay here, they are devices turning on)
    double appliancePower = 0;
    _appliances.forEach((k, v) { if (v['is_on']) appliancePower += (v['power_usage'] as num).toDouble(); });

    // 2. Calculate Ghost Power (Smooth drift, no jumps)
    // Drift by max +/- 20W per update
    double drift = (random.nextDouble() * 40) - 20; 
    _ghostPower = (_ghostPower + drift).clamp(300.0, 800.0); // Keep it realistic base load

    // 3. Total Power
    double totalPower = appliancePower + _ghostPower;
    
    // Voltage wobble
    baseVoltage = 118.0 + random.nextDouble() * 4.0 - 2.0;

    firestore.collection('users').doc(targetUserId).collection('live_stream').doc('current').set({
      't': DateTime.now().millisecondsSinceEpoch ~/ 1000,
      'rms_v': baseVoltage, 'rms_i': totalPower/baseVoltage, 'p': totalPower, 's': totalPower, 'pf': 1.0, 'thd': 0.0, 'harmonics': [0]
    });
    print("Wrote: ${totalPower.toStringAsFixed(0)} W -> $targetUserId");
  });

  // 3. HISTORY LOOP (Every 10s for demo speed)
  Timer.periodic(const Duration(seconds: 10), (timer) {
    if (targetUserId.isNotEmpty) {
      // Recalculate based on current state
      double appliancePower = 0;
      _appliances.forEach((k, v) { if (v['is_on']) appliancePower += (v['power_usage'] as num).toDouble(); });
      double total = appliancePower + _ghostPower;

      firestore.collection('users').doc(targetUserId).collection('history_hourly').add({
        't': DateTime.now().millisecondsSinceEpoch, 
        'p': total
      });
    }
  });

  // 4. APPLIANCE LOOP (7s)
  Timer.periodic(const Duration(seconds: 7), (timer) {
    if (targetUserId.isNotEmpty) {
      final key = ['tv', 'microwave', 'hvac'][random.nextInt(3)];
      _appliances[key]['is_on'] = !_appliances[key]['is_on'];
      firestore.collection('users').doc(targetUserId).collection('live_stream').doc('appliances').set({'appliance_list': _appliances.values.toList()});
    }
  });
}

// --- SMART BACKFILL FUNCTION ---
Future<void> _smartBackfillHistory(FirebaseFirestore db, String uid) async {
  print("Checking for data gaps...");
  
  // Get the most recent data point
  final snapshot = await db.collection('users').doc(uid).collection('history_hourly')
      .orderBy('t', descending: true).limit(1).get();
  
  DateTime startTime;
  
  if (snapshot.docs.isEmpty) {
    // Case 1: No data at all. Seed last 30 days.
    print("No history found. Seeding 30 days...");
    startTime = DateTime.now().subtract(const Duration(days: 30));
  } else {
    // Case 2: Data exists. Check when it stopped.
    final lastTimestamp = snapshot.docs.first.data()['t'];
    final lastDate = DateTime.fromMillisecondsSinceEpoch(lastTimestamp);
    
    // If last data was less than 1 hour ago, we are good.
    if (DateTime.now().difference(lastDate).inHours < 1) {
      print("History is up to date.");
      return;
    }
    
    print("Gap detected! Filling history from $lastDate to NOW...");
    startTime = lastDate;
  }

  // Generate hourly data from startTime -> Now
  final batch = db.batch();
  final now = DateTime.now();
  final random = Random();
  
  // Calculate hours to fill
  int hoursToFill = now.difference(startTime).inHours;
  
  // Safety cap to prevent freezing (max 720 hours = 30 days)
  if (hoursToFill > 720) hoursToFill = 720;

  for (int i = 1; i <= hoursToFill; i++) {
    final time = startTime.add(Duration(hours: i));
    final docRef = db.collection('users').doc(uid).collection('history_hourly').doc();
    
    // Realistic Curve Logic
    double power = 400.0 + random.nextDouble() * 100;
    
    // Time of Day Logic
    if (time.hour >= 7 && time.hour <= 9) power += 800; // Morning
    if (time.hour >= 17 && time.hour <= 22) power += 1500; // Evening
    if (time.hour >= 1 && time.hour <= 5) power = 300; // Night
    if (time.weekday >= 6) power *= 1.2; // Weekend

    batch.set(docRef, {
      't': time.millisecondsSinceEpoch,
      'p': power,
    });
  }
  
  await batch.commit();
  print("Backfill Complete! Added $hoursToFill hours of data.");
}