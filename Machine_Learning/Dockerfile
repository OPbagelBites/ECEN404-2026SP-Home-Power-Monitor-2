FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

# Set working directory
WORKDIR /app

# 1. Install dependencies first (better caching)
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# 2. Copy the necessary code and artifacts
COPY api.py .
COPY src/ ./src/
COPY models/ ./models/

# 3. Expose the port (Google Cloud Run expects 8080 by default)
ENV PORT=8080
EXPOSE 8080

# 4. Run the production server (Gunicorn)
CMD exec gunicorn --bind :$PORT --workers 1 --threads 8 --timeout 0 api:app