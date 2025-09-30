import pandas as pd
import json
import joblib
from pathlib import Path
from sklearn.model_selection import train_test_split, cross_val_score
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, classification_report
import seaborn as sns
import matplotlib.pyplot as plt

def train_model(data_path: Path, model_output_path: Path):
    """
    Loads features from a complex JSON file, processes nested data, validates the model
    using K-Fold Cross-Validation, trains a final model, evaluates it, and saves the result.
    """
    print(f"Loading data from {data_path}...")
    with open(data_path, 'r') as f_in:
        json_data = json.load(f_in)

    df = pd.DataFrame(json_data)
    print("Data loaded successfully. Initial shape:", df.shape)

    # --- Data Processing and Feature Engineering ---
    # 1. Extract the appliance name from the nested 'events' list to create the target column.
    df['appliance_type'] = df['events'].apply(
        lambda events_list: events_list[0]['appliance_identified'] if events_list else None
    )

    # 2. Flatten the nested 'h_i' dictionary into separate columns for the model.
    harmonics_df = df['h_i'].apply(pd.Series)
    harmonics_df = harmonics_df.rename(columns={
        '120': 'h2_i_norm', '180': 'h3_i_norm',
        '240': 'h4_i_norm', '300': 'h5_i_norm'
    })

    # 3. Combine the original dataframe with the new harmonic columns.
    df = pd.concat([df, harmonics_df], axis=1)

    # --- Feature Selection ---
    # These keys must match the final columns in the DataFrame.
    features = [
        'p', 's', 'pf_true', 'rms_i', 'thd_i', 'crest_i',
        'h2_i_norm', 'h3_i_norm', 'h4_i_norm', 'h5_i_norm'
    ]
    target = 'appliance_type'

    df.dropna(subset=[target], inplace=True)
    df[features] = df[features].fillna(0)

    X = df[features]
    y = df[target]

    # --- Train/Test Split ---
    # We split ONCE to create a final, locked-away test set for our "final exam".
    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )
    print(f"Total samples: {len(X)}. Training samples: {len(X_train)}. Test samples: {len(X_test)}.")

    # --- K-Fold Cross-Validation (for reliable performance estimate) ---
    # This gives us a more trustworthy idea of model performance than a single split.
    model_for_validation = RandomForestClassifier(n_estimators=100, random_state=42, n_jobs=-1)
    print("\n--- Performing 5-Fold Cross-Validation on the Training Set ---")
    scores = cross_val_score(model_for_validation, X_train, y_train, cv=5)
    print(f"Cross-validation scores for each fold: {scores}")
    print(f"Average cross-validation score: {scores.mean():.4f} (+/- {scores.std() * 2:.4f})")

    # --- Final Model Training ---
    # Now, we train a new model on the ENTIRE training set to learn from as much data as possible.
    print(f"\nTraining final model on {len(X_train)} samples...")
    final_model = RandomForestClassifier(n_estimators=100, random_state=42, n_jobs=-1)
    final_model.fit(X_train, y_train)
    print("Final model training complete.")

    # --- Final Model Evaluation (The "Final Exam") ---
    # We use the locked-away test set here for the first and only time.
    print("\n--- Final Model Performance on the Unseen Test Set ---")
    y_pred = final_model.predict(X_test)
    accuracy = accuracy_score(y_test, y_pred)
    print(f"Accuracy on Test Set: {accuracy:.4f}")
    
    print("\nClassification Report:")
    labels = sorted(list(set(y_test)))
    print(classification_report(y_test, y_pred, labels=labels))

    # --- Save the Final Trained Model ---
    print(f"\nSaving final model to {model_output_path}...")
    joblib.dump(final_model, model_output_path)
    print("Model saved successfully as a .joblib file.")
    
    # --- Feature Importance Analysis ---
    plt.figure(figsize=(10, 8))
    feature_imp = pd.Series(final_model.feature_importances_, index=features).sort_values(ascending=False)
    sns.barplot(x=feature_imp, y=feature_imp.index)
    plt.title("Feature Importance for Final Model")
    plt.xlabel("Importance Score")
    plt.ylabel("Features")
    plt.tight_layout()
    plt.savefig('feature_importance.png')
    print("\nFeature importance plot saved to feature_importance.png")


if __name__ == '__main__':
    project_root = Path(__file__).parent.parent.parent
    processed_data_path = project_root / 'data' / 'processed' / 'appliance_features.json'
    model_path = project_root / 'models' / 'appliance_model.joblib'

    model_path.parent.mkdir(parents=True, exist_ok=True)
    
    train_model(processed_data_path, model_path)

