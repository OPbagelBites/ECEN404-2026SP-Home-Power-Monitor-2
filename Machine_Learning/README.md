# Ohm Sweet Home: ML Appliance Identification 💡

## Overview

This repository contains the machine learning pipeline for the "Ohm Sweet Home" project, designed to identify individual household appliances from a single, whole-house power meter using Non-Intrusive Load Monitoring (NILM).

---

## Project Description

The goal of this project is to disaggregate a home's total energy consumption into appliance-specific usage. By analyzing high-frequency electrical data (voltage and current), our machine learning model can detect the unique "fingerprints" of different devices as they turn on and off. This provides homeowners with detailed insights, helping them save energy and reduce their electricity bills.

This repository covers the complete ML workflow: from processing raw electrical data to training a classification model and evaluating its performance.

---

## Features ✨

* **Feature Extraction:** Processes raw voltage/current data to extract key electrical features like active power, power factor, harmonic distortion, and crest factor.
* **Model Training:** Trains a Random Forest classifier (or other models) to recognize the signatures of different appliances.
* **Appliance Classification:** Uses the trained model to predict which appliance corresponds to a specific electrical event.
* **Modular & Scalable:** Organized in a clean, professional project structure that is easy to understand and extend.

---

## Project Structure 📁

The repository is organized to separate data, notebooks, and source code for clarity and maintainability.