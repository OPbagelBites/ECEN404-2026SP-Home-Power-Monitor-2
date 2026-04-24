import 'package:flutter/material.dart';
import 'package:cloud_firestore/cloud_firestore.dart'; // CHANGED: Use Firestore
import 'package:firebase_auth/firebase_auth.dart';

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  final TextEditingController _priceController = TextEditingController();
  String _savedPrice = "Not set";
  final User? user = FirebaseAuth.instance.currentUser;

  @override
  void initState() {
    super.initState();
    _loadPrice();
  }

  // NEW: Load from Firebase
  Future<void> _loadPrice() async {
    if (user == null) return;
    
    final doc = await FirebaseFirestore.instance
        .collection('users')
        .doc(user!.uid)
        .collection('settings')
        .doc('energy_cost')
        .get();

    if (doc.exists && mounted) {
      setState(() {
        double price = doc.data()?['price_per_kwh'] ?? 0.0;
        _savedPrice = price.toString();
        _priceController.text = price.toString();
      });
    }
  }

  // NEW: Save to Firebase
  Future<void> _savePrice() async {
    if (user == null) return;
    
    double? newPrice = double.tryParse(_priceController.text);
    if (newPrice != null) {
      await FirebaseFirestore.instance
          .collection('users')
          .doc(user!.uid)
          .collection('settings')
          .doc('energy_cost')
          .set({'price_per_kwh': newPrice}); // Writes to cloud

      setState(() { _savedPrice = newPrice.toString(); });
      
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Saved to Cloud!')));
    }
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(16.0),
      child: ListView(
        children: [
          const Text('Energy Settings', style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold)),
          const SizedBox(height: 20),
          Text('Logged in as: ${user?.email}', style: const TextStyle(color: Colors.grey)),
          const SizedBox(height: 20),
          TextField(controller: _priceController, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'Price per kWh (\$)', border: OutlineInputBorder())),
          const SizedBox(height: 10),
          ElevatedButton(onPressed: _savePrice, child: const Text('Save Price')),
          const SizedBox(height: 20),
          const Divider(),
          Center(child: Text('Current Saved Rate: \$$_savedPrice / kWh', style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: Colors.deepPurple))),
          const SizedBox(height: 40),
          OutlinedButton(onPressed: () => FirebaseAuth.instance.signOut(), style: OutlinedButton.styleFrom(foregroundColor: Colors.red), child: const Text('Sign Out')),
        ],
      ),
    );
  }
}