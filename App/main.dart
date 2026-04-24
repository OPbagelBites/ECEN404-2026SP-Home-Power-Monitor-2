import 'package:flutter/material.dart';
import 'package:firebase_core/firebase_core.dart';
import 'firebase_options.dart';
import 'auth_gate.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(
    options: DefaultFirebaseOptions.currentPlatform,
  );
  runApp(const HomePowerMonitorApp());
}

class HomePowerMonitorApp extends StatelessWidget {
  const HomePowerMonitorApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'OhmSweetHome',
      theme: ThemeData(
        // Define the global color palette
        colorScheme: const ColorScheme.light(
          primary: Color(0xFF0D1B2A), // Dark Navy Blue
          secondary: Color(0xFF42A5F5), // Sky Blue (Dodger Blue)
          surface: Colors.white,
          onPrimary: Colors.white,
          onSurface: Color(0xFF0D1B2A), // Navy text
        ),
        scaffoldBackgroundColor: Colors.white,
        useMaterial3: true,
        
        // Style all cards to be white with a subtle shadow
        cardTheme: CardThemeData(
          color: Colors.white,
          elevation: 4,
          shadowColor: Colors.black26,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
          ),
        ),
        
        // Style the App Bar
        appBarTheme: const AppBarTheme(
          backgroundColor: Colors.white,
          foregroundColor: Color(0xFF0D1B2A), // Navy text
          elevation: 0,
          centerTitle: true,
          titleTextStyle: TextStyle(
            color: Color(0xFF0D1B2A),
            fontSize: 22,
            fontWeight: FontWeight.bold,
            letterSpacing: 1.0,
          )
        ),
        
        // Style Buttons
        elevatedButtonTheme: ElevatedButtonThemeData(
          style: ElevatedButton.styleFrom(
            backgroundColor: const Color(0xFF42A5F5), // Sky Blue buttons
            foregroundColor: Colors.white, // White text
            elevation: 2,
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
            padding: const EdgeInsets.symmetric(vertical: 16),
          ),
        ),
      ),
      home: const AuthGate(),
      debugShowCheckedModeBanner: false,
    );
  }
}